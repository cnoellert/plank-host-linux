/**
 * @file src/clipboard_protocol.h
 * @brief Shared validation helpers for PLANK clipboard frames.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

extern "C" {
#include <moonlight-common-c/src/plank.h>
}

#include "utility.h"

namespace stream::clipboard {
  class inbox_t {
  public:
    void store(std::vector<std::uint8_t> text) {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_ = std::move(text);
    }

    std::optional<std::vector<std::uint8_t>> take() {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!pending_) {
        return std::nullopt;
      }
      auto text = std::move(pending_);
      pending_.reset();
      return text;
    }

  private:
    std::mutex mutex_;
    std::optional<std::vector<std::uint8_t>> pending_;
  };

  inline bool valid_utf8(const std::uint8_t *data, std::size_t size) {
    if (data == nullptr || size == 0) {
      return false;
    }
    for (std::size_t index = 0; index < size;) {
      const auto byte = data[index];
      if (byte <= 0x7F) {
        if (byte == 0) {
          return false;
        }
        ++index;
        continue;
      }
      const auto continuation = [&](std::size_t offset) {
        return index + offset < size && (data[index + offset] & 0xC0) == 0x80;
      };
      if (byte >= 0xC2 && byte <= 0xDF) {
        if (!continuation(1)) {
          return false;
        }
        index += 2;
        continue;
      }
      if (byte >= 0xE0 && byte <= 0xEF) {
        if (!continuation(1) || !continuation(2)) {
          return false;
        }
        const auto second = data[index + 1];
        if ((byte == 0xE0 && second < 0xA0) ||
            (byte == 0xED && second > 0x9F)) {
          return false;
        }
        index += 3;
        continue;
      }
      if (byte >= 0xF0 && byte <= 0xF4) {
        if (!continuation(1) || !continuation(2) || !continuation(3)) {
          return false;
        }
        const auto second = data[index + 1];
        if ((byte == 0xF0 && second < 0x90) ||
            (byte == 0xF4 && second > 0x8F)) {
          return false;
        }
        index += 4;
        continue;
      }
      return false;
    }
    return true;
  }

  inline std::uint32_t chunk_size(const PLANK_CLIPBOARD_WIRE_HEADER &wire) {
    std::uint32_t value {};
    std::memcpy(&value, &wire.chunkSize, sizeof(value));
    return util::endian::little(value);
  }

  inline bool payload_size_matches(const PLANK_CLIPBOARD_WIRE_HEADER &wire,
                                   std::size_t payload_size,
                                   std::uint32_t maximum_chunk_size) {
    const auto size = chunk_size(wire);
    return size > 0 &&
           size <= maximum_chunk_size &&
           payload_size == sizeof(wire) + size;
  }

  enum class receive_status_e {
    rejected,
    incomplete,
    ignored,
    complete,
  };

  struct receive_result_t {
    receive_status_e status {receive_status_e::rejected};
    std::uint64_t generation = 0;
    std::vector<std::uint8_t> text;
  };

  class receiver_t {
  public:
    void reset() {
      reset_assembly();
      last_generation_ = 0;
    }

    receive_result_t append(const std::uint8_t *payload,
                            std::size_t payload_size,
                            std::uint32_t maximum_chunk_size) {
      if (payload == nullptr || payload_size < sizeof(PLANK_CLIPBOARD_WIRE_HEADER)) {
        reset_assembly();
        return {};
      }

      PLANK_CLIPBOARD_WIRE_HEADER wire {};
      std::memcpy(&wire, payload, sizeof(wire));
      if (!payload_size_matches(wire, payload_size, maximum_chunk_size)) {
        reset_assembly();
        return {};
      }

      const auto magic = value(wire.magic);
      const auto version = value(wire.version);
      const auto reserved = value(wire.reserved);
      const auto flags = value(wire.flags);
      const auto generation = value(wire.generation);
      const auto total_size = value(wire.totalSize);
      const auto chunk_offset = value(wire.chunkOffset);
      const auto current_chunk_size = value(wire.chunkSize);
      constexpr std::uint32_t known_flags =
        PLANK_CLIPBOARD_FLAG_FIRST_CHUNK | PLANK_CLIPBOARD_FLAG_LAST_CHUNK;

      if (magic != PLANK_CLIPBOARD_WIRE_MAGIC ||
          version != PLANK_CLIPBOARD_WIRE_VERSION ||
          reserved != 0 ||
          generation == 0 ||
          (flags & ~known_flags) != 0 ||
          total_size == 0 ||
          total_size > PLANK_CLIPBOARD_MAX_TEXT_SIZE ||
          chunk_offset > total_size ||
          current_chunk_size > total_size - chunk_offset) {
        reset_assembly();
        return {};
      }

      if (generation <= last_generation_) {
        return {receive_status_e::ignored, generation, {}};
      }

      if ((flags & PLANK_CLIPBOARD_FLAG_FIRST_CHUNK) != 0) {
        if (chunk_offset != 0) {
          reset_assembly();
          return {};
        }
        active_ = true;
        generation_ = generation;
        total_size_ = total_size;
        next_offset_ = 0;
        bytes_.assign(total_size, 0);
      }

      if (!active_ ||
          generation_ != generation ||
          total_size_ != total_size ||
          next_offset_ != chunk_offset) {
        reset_assembly();
        return {};
      }

      std::memcpy(
        bytes_.data() + chunk_offset,
        payload + sizeof(wire),
        current_chunk_size
      );
      next_offset_ += current_chunk_size;

      if ((flags & PLANK_CLIPBOARD_FLAG_LAST_CHUNK) == 0) {
        if (next_offset_ == total_size_) {
          reset_assembly();
          return {};
        }
        return {receive_status_e::incomplete, generation, {}};
      }

      if (next_offset_ != total_size_ || !valid_utf8(bytes_.data(), bytes_.size())) {
        reset_assembly();
        return {};
      }

      receive_result_t result {
        receive_status_e::complete,
        generation,
        std::move(bytes_),
      };
      last_generation_ = generation;
      reset_assembly();
      return result;
    }

  private:
    template<typename T>
    static T value(const T &wire_value) {
      T result {};
      std::memcpy(&result, &wire_value, sizeof(result));
      return util::endian::little(result);
    }

    void reset_assembly() {
      active_ = false;
      generation_ = 0;
      total_size_ = 0;
      next_offset_ = 0;
      bytes_.clear();
    }

    bool active_ = false;
    std::uint64_t generation_ = 0;
    std::uint64_t last_generation_ = 0;
    std::uint32_t total_size_ = 0;
    std::uint32_t next_offset_ = 0;
    std::vector<std::uint8_t> bytes_;
  };
}  // namespace stream::clipboard
