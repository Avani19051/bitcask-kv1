#include "kvstore.h"
#include <ios>
#include <filesystem>
#include <fstream>

// A special value_size that means "this record is a deletion (tombstone)".
// Real values can never be this big in our toy store, so it's safe as a marker.
static const uint32_t TOMBSTONE = 0xFFFFFFFF;

// --- helpers to read/write a 32-bit number as raw bytes -----------------------
// We store sizes as fixed 4-byte little-endian numbers so the file format is
// predictable and we can always know exactly how many bytes to read.

static void writeUint32(std::ostream& os, uint32_t v) {
    char buf[4];
    buf[0] = static_cast<char>(v & 0xFF);
    buf[1] = static_cast<char>((v >> 8) & 0xFF);
    buf[2] = static_cast<char>((v >> 16) & 0xFF);
    buf[3] = static_cast<char>((v >> 24) & 0xFF);
    os.write(buf, 4);
}

static bool readUint32(std::istream& is, uint32_t& v) {
    char buf[4];
    if (!is.read(buf, 4)) return false;
    v = (static_cast<uint32_t>(static_cast<unsigned char>(buf[0]))) |
        (static_cast<uint32_t>(static_cast<unsigned char>(buf[1])) << 8) |
        (static_cast<uint32_t>(static_cast<unsigned char>(buf[2])) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(buf[3])) << 24);
    return true;
}

// --- constructor: open the file --------------------------------------------
KVStore::KVStore(const std::string& path) : path_(path) {
    // Open the file for reading AND writing, in binary mode.
    // We try to open an existing file first; if it doesn't exist, create it.
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) {
        // File didn't exist yet -> create it, then reopen in read+write mode.
        std::ofstream create(path_, std::ios::binary);
        create.close();
        file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    }
    // Stage 2: replay the file to rebuild the index so data survives restarts.
    recover();
}

// --- recover: rebuild the index by replaying the whole file ----------------
void KVStore::recover() {
    file_.clear();
    file_.seekg(0, std::ios::beg); // start reading from the very beginning

    while (true) {
        // Remember where this record starts (we need it to locate the value).
        uint64_t record_start = static_cast<uint64_t>(file_.tellg());

        uint32_t key_size, value_size;
        if (!readUint32(file_, key_size)) break;   // no more records -> done
        if (!readUint32(file_, value_size)) break; // file ended mid-record -> stop

        // Read the key bytes.
        std::string key(key_size, '\0');
        if (!file_.read(&key[0], key_size)) break;

        if (value_size == TOMBSTONE) {
            // This record is a deletion: remove the key from our live view.
            index_.erase(key);
        } else {
            // Normal record: the value sits right after the key. Its offset is
            // record_start + 4 (key_size) + 4 (value_size) + key_size.
            uint64_t value_offset = record_start + 8 + key_size;
            index_[key] = IndexEntry{ value_offset, value_size };

            // Skip over the value bytes to reach the next record.
            file_.seekg(static_cast<std::streamoff>(value_size), std::ios::cur);
        }
    }

    file_.clear(); // clear the EOF flag so normal reads/writes work afterwards
}

KVStore::~KVStore() {
    if (file_.is_open()) file_.close();
}

// --- SET: append a record, update the index --------------------------------
void KVStore::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mu_);
    // Always write at the end of the file (append-only).
    file_.clear();                 // clear any leftover error/eof flags
    file_.seekp(0, std::ios::end); // move the WRITE position to end of file

    writeUint32(file_, static_cast<uint32_t>(key.size()));   // key length
    writeUint32(file_, static_cast<uint32_t>(value.size())); // value length
    file_.write(key.data(), key.size());                     // the key bytes

    // Remember where the value bytes are about to land BEFORE we write them.
    uint64_t value_offset = static_cast<uint64_t>(file_.tellp());
    file_.write(value.data(), value.size());                 // the value bytes
    file_.flush();                                           // push to the OS

    // Point the index at this newest value for this key.
    index_[key] = IndexEntry{ value_offset,
                              static_cast<uint32_t>(value.size()) };
}

// --- GET: look up offset, jump there, read the value -----------------------
bool KVStore::get(const std::string& key, std::string& out) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = index_.find(key);
    if (it == index_.end()) return false; // key never set, or was deleted

    const IndexEntry& e = it->second;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(e.value_offset), std::ios::beg);

    out.resize(e.value_size);
    if (!file_.read(&out[0], e.value_size)) return false;
    return true;
}

// --- DEL: append a tombstone, forget the key -------------------------------
void KVStore::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    file_.clear();
    file_.seekp(0, std::ios::end);

    writeUint32(file_, static_cast<uint32_t>(key.size()));
    writeUint32(file_, TOMBSTONE);        // value_size == TOMBSTONE means "deleted"
    file_.write(key.data(), key.size());  // no value bytes follow a tombstone
    file_.flush();

    index_.erase(key); // the key is gone from our live view
}

// --- fileSize: how big the data file currently is ---------------------------
uint64_t KVStore::fileSize() {
    std::lock_guard<std::mutex> lock(mu_);
    file_.clear();
    file_.seekg(0, std::ios::end);
    return static_cast<uint64_t>(file_.tellg());
}

// --- compact: rewrite the file with only the latest value per live key ------
//
// Why this is needed: every set/del only APPENDS. So the file accumulates old,
// superseded records and tombstones that no longer matter. The in-memory index_
// already points only at the LATEST value of each LIVE key (deleted keys were
// erased from it). So "the live data" is exactly: for each key in index_, the
// value it points to. Compaction writes just those into a fresh file.
//
// Crash safety: we write everything to a NEW temp file first, fully flush it,
// and only then atomically replace the old file. If a crash happens partway,
// the original file is still intact and untouched.
void KVStore::compact() {
    std::lock_guard<std::mutex> lock(mu_);
    namespace fs = std::filesystem;
    const std::string tmp_path = path_ + ".compact";

    // The index we'll have AFTER compaction (new offsets in the new file).
    std::unordered_map<std::string, IndexEntry> new_index;

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);

        for (const auto& kv : index_) {
            const std::string& key = kv.first;
            const IndexEntry& e = kv.second;

            // Read this key's current value out of the OLD file.
            std::string value(e.value_size, '\0');
            file_.clear();
            file_.seekg(static_cast<std::streamoff>(e.value_offset), std::ios::beg);
            file_.read(&value[0], e.value_size);

            // Write a fresh, single record for it into the new file.
            writeUint32(out, static_cast<uint32_t>(key.size()));
            writeUint32(out, e.value_size);
            out.write(key.data(), key.size());

            uint64_t new_value_offset = static_cast<uint64_t>(out.tellp());
            out.write(value.data(), value.size());

            new_index[key] = IndexEntry{ new_value_offset, e.value_size };
        }

        out.flush();
        out.close(); // make sure all bytes are on disk before we swap
    }

    // Swap the files. Close our handle so the OS lets us replace the file.
    file_.close();
    fs::rename(tmp_path, path_); // atomically replaces the old data file

    // Reopen the (now compacted) file and switch to the new offsets.
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    index_ = std::move(new_index);
}
