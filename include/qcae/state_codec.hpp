#pragma once
#include "qcae/model.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace qcae::state_codec {
inline constexpr std::size_t max_bytes = 64u * 1024u * 1024u;
class CodecError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
class Writer {
  public:
    void number(std::uint64_t value);
    void real(double value);
    void text(std::string_view value);
    void boolean(bool value);
    const std::string& bytes() const {
        return bytes_;
    }
    std::string take() {
        return std::move(bytes_);
    }

  private:
    std::string bytes_;
};
class Reader {
  public:
    explicit Reader(std::string_view bytes);
    std::uint64_t number();
    double real();
    std::string text();
    bool boolean();
    void finish() const;
    std::size_t remaining() const {
        return bytes_.size() - offset_;
    }

  private:
    std::string_view bytes_;
    std::size_t offset_{};
};
void write_model(Writer&, const Model&);
Model read_model(Reader&, std::size_t max_entities = 500000, std::size_t max_sources = 500000);
std::string encode_model(const Model&);
Model decode_model(std::string_view);
} // namespace qcae::state_codec
