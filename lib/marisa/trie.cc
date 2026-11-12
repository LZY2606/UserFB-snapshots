#include "marisa/trie.h"

#include <memory>
#include <stdexcept>
#include <vector>

#include "marisa/grimoire/trie.h"
#include "marisa/iostream.h"
#include "marisa/stdio.h"

namespace marisa {

namespace {

// Decodes `query` as strict UTF-8 into codepoints. Overlong forms, UTF-16
// surrogates, values above U+10FFFF and truncated sequences are rejected
// with std::invalid_argument. Embedded '\0' bytes decode to U+0000.
std::vector<char32_t> decode_fuzzy_query(std::string_view query) {
  std::vector<char32_t> cps;
  std::size_t i = 0;
  while (i < query.size()) {
    const uint8_t lead = static_cast<uint8_t>(query[i]);
    char32_t cp;
    std::size_t extra;
    char32_t min_cp;
    if (lead < 0x80) {
      cp = lead;
      extra = 0;
      min_cp = 0;
    } else if (lead < 0xC2) {
      MARISA_THROW(std::invalid_argument, "query is not valid UTF-8");
    } else if (lead < 0xE0) {
      cp = lead & 0x1F;
      extra = 1;
      min_cp = 0x80;
    } else if (lead < 0xF0) {
      cp = lead & 0x0F;
      extra = 2;
      min_cp = 0x800;
    } else if (lead < 0xF5) {
      cp = lead & 0x07;
      extra = 3;
      min_cp = 0x10000;
    } else {
      MARISA_THROW(std::invalid_argument, "query is not valid UTF-8");
    }
    MARISA_THROW_IF(query.size() - i - 1 < extra, std::invalid_argument);
    for (std::size_t j = 1; j <= extra; ++j) {
      const uint8_t cont = static_cast<uint8_t>(query[i + j]);
      MARISA_THROW_IF((cont & 0xC0) != 0x80, std::invalid_argument);
      cp = (cp << 6) | (cont & 0x3F);
    }
    MARISA_THROW_IF((cp < min_cp) || ((cp & 0xFFFFF800) == 0xD800) ||
                        (cp > 0x10FFFF),
                    std::invalid_argument);
    cps.push_back(cp);
    i += extra + 1;
  }
  return cps;
}

}  // namespace

Trie::Trie() = default;

Trie::~Trie() = default;

Trie::Trie(Trie &&other) noexcept = default;

Trie &Trie::operator=(Trie &&other) noexcept = default;

void Trie::build(Keyset &keyset, int config_flags) {
  std::unique_ptr<grimoire::LoudsTrie> temp(new grimoire::LoudsTrie);

  temp->build(keyset, config_flags);
  trie_.swap(temp);
}

void Trie::mmap(const char *filename, int flags) {
  MARISA_THROW_IF(filename == nullptr, std::invalid_argument);

  std::unique_ptr<grimoire::LoudsTrie> temp(new grimoire::LoudsTrie);

  grimoire::Mapper mapper;
  mapper.open(filename, flags);
  temp->map(mapper);
  trie_.swap(temp);
}

void Trie::map(const void *ptr, std::size_t size) {
  MARISA_THROW_IF((ptr == nullptr) && (size != 0), std::invalid_argument);

  std::unique_ptr<grimoire::LoudsTrie> temp(new grimoire::LoudsTrie);

  grimoire::Mapper mapper;
  mapper.open(ptr, size);
  temp->map(mapper);
  trie_.swap(temp);
}

void Trie::load(const char *filename) {
  MARISA_THROW_IF(filename == nullptr, std::invalid_argument);

  std::unique_ptr<grimoire::LoudsTrie> temp(new grimoire::LoudsTrie);

  grimoire::Reader reader;
  reader.open(filename);
  temp->read(reader);
  trie_.swap(temp);
}

void Trie::read(int fd) {
  MARISA_THROW_IF(fd == -1, std::invalid_argument);

  std::unique_ptr<grimoire::LoudsTrie> temp(new grimoire::LoudsTrie);

  grimoire::Reader reader;
  reader.open(fd);
  temp->read(reader);
  trie_.swap(temp);
}

void Trie::save(const char *filename) const {
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  MARISA_THROW_IF(filename == nullptr, std::invalid_argument);

  grimoire::Writer writer;
  writer.open(filename);
  trie_->write(writer);
}

void Trie::write(int fd) const {
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  MARISA_THROW_IF(fd == -1, std::invalid_argument);

  grimoire::Writer writer;
  writer.open(fd);
  trie_->write(writer);
}

bool Trie::lookup(Agent &agent) const {
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  if (!agent.has_state()) {
    agent.init_state();
  }
  return trie_->lookup(agent);
}

void Trie::reverse_lookup(Agent &agent) const {
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  if (!agent.has_state()) {
    agent.init_state();
  }
  trie_->reverse_lookup(agent);
}

bool Trie::common_prefix_search(Agent &agent) const {
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  if (!agent.has_state()) {
    agent.init_state();
  }
  return trie_->common_prefix_search(agent);
}

bool Trie::predictive_search(Agent &agent) const {
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  if (!agent.has_state()) {
    agent.init_state();
  }
  return trie_->predictive_search(agent);
}

std::vector<FuzzyMatch> Trie::fuzzy_search(std::string_view query,
                                           unsigned max_edits,
                                           FuzzyStats *stats) const {
  if (stats != nullptr) {
    stats->visited_nodes = 0;
    stats->decoded_codepoints = 0;
  }
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  MARISA_THROW_IF(max_edits > 2, std::out_of_range);
  return trie_->fuzzy_search(decode_fuzzy_query(query), max_edits, stats);
}

std::size_t Trie::num_tries() const {
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  return trie_->num_tries();
}

std::size_t Trie::num_keys() const {
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  return trie_->num_keys();
}

std::size_t Trie::num_nodes() const {
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  return trie_->num_nodes();
}

TailMode Trie::tail_mode() const {
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  return trie_->tail_mode();
}

NodeOrder Trie::node_order() const {
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  return trie_->node_order();
}

bool Trie::empty() const {
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  return trie_->empty();
}

std::size_t Trie::size() const {
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  return trie_->size();
}

std::size_t Trie::total_size() const {
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  return trie_->total_size();
}

std::size_t Trie::io_size() const {
  MARISA_THROW_IF(trie_ == nullptr, std::logic_error);
  return trie_->io_size();
}

void Trie::clear() noexcept {
  Trie().swap(*this);
}

void Trie::swap(Trie &rhs) noexcept {
  trie_.swap(rhs.trie_);
}

}  // namespace marisa

#include <iostream>

namespace marisa {

class TrieIO {
 public:
  static void fread(std::FILE *file, Trie *trie) {
    MARISA_THROW_IF(trie == nullptr, std::invalid_argument);

    std::unique_ptr<grimoire::LoudsTrie> temp(new grimoire::LoudsTrie);

    grimoire::Reader reader;
    reader.open(file);
    temp->read(reader);
    trie->trie_.swap(temp);
  }
  static void fwrite(std::FILE *file, const Trie &trie) {
    MARISA_THROW_IF(file == nullptr, std::invalid_argument);
    MARISA_THROW_IF(trie.trie_ == nullptr, std::logic_error);
    grimoire::Writer writer;
    writer.open(file);
    trie.trie_->write(writer);
  }

  static std::istream &read(std::istream &stream, Trie *trie) {
    MARISA_THROW_IF(trie == nullptr, std::invalid_argument);

    std::unique_ptr<grimoire::LoudsTrie> temp(new grimoire::LoudsTrie);

    grimoire::Reader reader;
    reader.open(stream);
    temp->read(reader);
    trie->trie_.swap(temp);
    return stream;
  }
  static std::ostream &write(std::ostream &stream, const Trie &trie) {
    MARISA_THROW_IF(trie.trie_ == nullptr, std::logic_error);
    grimoire::Writer writer;
    writer.open(stream);
    trie.trie_->write(writer);
    return stream;
  }
};

void fread(std::FILE *file, Trie *trie) {
  MARISA_THROW_IF(file == nullptr, std::invalid_argument);
  MARISA_THROW_IF(trie == nullptr, std::invalid_argument);
  TrieIO::fread(file, trie);
}

void fwrite(std::FILE *file, const Trie &trie) {
  MARISA_THROW_IF(file == nullptr, std::invalid_argument);
  TrieIO::fwrite(file, trie);
}

std::istream &read(std::istream &stream, Trie *trie) {
  MARISA_THROW_IF(trie == nullptr, std::invalid_argument);
  return TrieIO::read(stream, trie);
}

std::ostream &write(std::ostream &stream, const Trie &trie) {
  return TrieIO::write(stream, trie);
}

std::istream &operator>>(std::istream &stream, Trie &trie) {
  return read(stream, &trie);
}

std::ostream &operator<<(std::ostream &stream, const Trie &trie) {
  return write(stream, trie);
}

}  // namespace marisa
