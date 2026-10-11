#pragma once
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// File layout: FileHeader, nsyms x SymInfo, nevents x Event.
struct FileHeader {
  char magic[8];
  uint32_t version;
  uint32_t nsyms;
  uint64_t nevents;
};
struct SymInfo {
  char name[8];     // space padded, as in ITCH
  Price ref_price;  // first regular-session execution price; centres the flat ladder
  uint32_t reserved;
};
static_assert(sizeof(FileHeader) == 24 && sizeof(SymInfo) == 16);
inline constexpr char kMagic[8] = {'L', 'O', 'B', 'E', 'V', '0', '1', 0};

inline std::string sym_name(const SymInfo& s) {
  std::string n(s.name, 8);
  return n.substr(0, n.find_last_not_of(' ') + 1);
}

struct EventFile {
  std::vector<SymInfo> syms;
  std::vector<Event> events;
};

inline EventFile read_events(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) throw std::runtime_error("cannot open " + path);
  FileHeader h;
  EventFile ef;
  bool ok = std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, kMagic, 8) == 0;
  if (ok) {
    ef.syms.resize(h.nsyms);
    ef.events.resize(h.nevents);
    ok = std::fread(ef.syms.data(), sizeof(SymInfo), h.nsyms, f) == h.nsyms &&
         std::fread(ef.events.data(), sizeof(Event), h.nevents, f) == h.nevents;
  }
  std::fclose(f);
  if (!ok) throw std::runtime_error("bad event file " + path);
  return ef;
}

class EventWriter {
  FILE* f_;
  std::vector<Event> buf_;
  uint64_t n_ = 0;
  uint32_t nsyms_;

  void drain() {
    std::fwrite(buf_.data(), sizeof(Event), buf_.size(), f_);
    buf_.clear();
  }

 public:
  EventWriter(const std::string& path, uint32_t nsyms) : f_(std::fopen(path.c_str(), "wb")), nsyms_(nsyms) {
    if (!f_) throw std::runtime_error("cannot write " + path);
    std::vector<char> zeros(sizeof(FileHeader) + nsyms * sizeof(SymInfo), 0);
    std::fwrite(zeros.data(), 1, zeros.size(), f_);  // placeholder, rewritten by finish()
    buf_.reserve(1 << 16);
  }
  void write(const Event& e) {
    buf_.push_back(e);
    ++n_;
    if (buf_.size() == buf_.capacity()) drain();
  }
  uint64_t count() const { return n_; }
  void finish(const std::vector<SymInfo>& syms) {
    drain();
    FileHeader h{};
    std::memcpy(h.magic, kMagic, 8);
    h.version = 1;
    h.nsyms = nsyms_;
    h.nevents = n_;
    std::fseek(f_, 0, SEEK_SET);
    std::fwrite(&h, sizeof h, 1, f_);
    std::fwrite(syms.data(), sizeof(SymInfo), syms.size(), f_);
    std::fclose(f_);
    f_ = nullptr;
  }
};

}  // namespace lob
