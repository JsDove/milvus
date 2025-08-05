#pragma once

#include "BooPHF.h"
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <limits>
#include <cstring>
#include <fstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <cassert>

namespace primaryIndex
{

    // Forward declaration
    template <typename Hasher_t>
    class PrimaryIndex;

    // Bit-packed array for efficient storage of small integers
    class BitPackedArray
    {
    private:
        std::vector<uint64_t> data_;
        uint32_t bits_per_value_;
        uint32_t values_per_word_;
        uint32_t size_;
        uint64_t mask_;

    public:
        BitPackedArray() : bits_per_value_(0), values_per_word_(0), size_(0), mask_(0) {}

        // Initialize with the maximum value to determine bits needed
        void init(uint64_t max_value, uint32_t num_values)
        {
            size_ = num_values;

            // Calculate bits needed for the maximum value
            bits_per_value_ = 1;
            uint64_t temp = max_value;
            while (temp >>= 1)
                bits_per_value_++;

            // Calculate how many values fit in a 64-bit word
            values_per_word_ = 64 / bits_per_value_;

            // Create mask for extracting values
            mask_ = (1ULL << bits_per_value_) - 1;

            // Calculate storage size
            uint32_t num_words = (num_values + values_per_word_ - 1) / values_per_word_;
            data_.resize(num_words, 0);
        }

        // Set a value at index
        void set(uint32_t index, uint64_t value)
        {
            if (index >= size_)
                return;

            uint32_t word_index = index / values_per_word_;
            uint32_t bit_offset = (index % values_per_word_) * bits_per_value_;

            // Clear the bits for this value
            data_[word_index] &= ~(mask_ << bit_offset);
            // Set the new value
            data_[word_index] |= (value & mask_) << bit_offset;
        }

        // Get a value at index
        uint64_t get(uint32_t index) const
        {
            if (index >= size_)
                return 0;

            uint32_t word_index = index / values_per_word_;
            uint32_t bit_offset = (index % values_per_word_) * bits_per_value_;

            return (data_[word_index] >> bit_offset) & mask_;
        }

        // Get the size
        uint32_t size() const { return size_; }

        // Serialization methods for mmap support
        void serialize(std::ofstream &out) const
        {
            out.write(reinterpret_cast<const char *>(&bits_per_value_), sizeof(bits_per_value_));
            out.write(reinterpret_cast<const char *>(&values_per_word_), sizeof(values_per_word_));
            out.write(reinterpret_cast<const char *>(&size_), sizeof(size_));
            out.write(reinterpret_cast<const char *>(&mask_), sizeof(mask_));

            uint32_t data_size = data_.size();
            out.write(reinterpret_cast<const char *>(&data_size), sizeof(data_size));
            out.write(reinterpret_cast<const char *>(data_.data()), data_size * sizeof(uint64_t));
        }

        void deserialize(std::ifstream &in)
        {
            in.read(reinterpret_cast<char *>(&bits_per_value_), sizeof(bits_per_value_));
            in.read(reinterpret_cast<char *>(&values_per_word_), sizeof(values_per_word_));
            in.read(reinterpret_cast<char *>(&size_), sizeof(size_));
            in.read(reinterpret_cast<char *>(&mask_), sizeof(mask_));

            uint32_t data_size;
            in.read(reinterpret_cast<char *>(&data_size), sizeof(data_size));
            data_.resize(data_size);
            in.read(reinterpret_cast<char *>(data_.data()), data_size * sizeof(uint64_t));
        }

        void deserialize_from_mmap(const char *data, size_t &offset)
        {
            memcpy(&bits_per_value_, data + offset, sizeof(bits_per_value_));
            offset += sizeof(bits_per_value_);
            memcpy(&values_per_word_, data + offset, sizeof(values_per_word_));
            offset += sizeof(values_per_word_);
            memcpy(&size_, data + offset, sizeof(size_));
            offset += sizeof(size_);
            memcpy(&mask_, data + offset, sizeof(mask_));
            offset += sizeof(mask_);

            uint32_t data_size;
            memcpy(&data_size, data + offset, sizeof(data_size));
            offset += sizeof(data_size);

            data_.resize(data_size);
            memcpy(data_.data(), data + offset, data_size * sizeof(uint64_t));
            offset += data_size * sizeof(uint64_t);
        }
    };

    // Custom hasher for strings (can be extended for other types)
    class StringHasher
    {
    public:
        typedef std::string Item;
        typedef std::pair<uint64_t, uint64_t> hash_pair_t;

    public:
        hash_pair_t operator()(const Item &key) const
        {
            hash_pair_t result;
            // Use different seeds for the two hash values
            result.first = murmurHash3_64(key, 0xAAAAAAAA55555555ULL);
            result.second = murmurHash3_64(key, 0x33333333CCCCCCCCULL);
            return result;
        }

    private:
        // MurmurHash3 implementation
        uint64_t murmurHash3_64(const std::string &key, uint64_t seed) const
        {
            const uint64_t c1 = 0x87c37b91114253d5ULL;
            const uint64_t c2 = 0x4cf5ad432745937fULL;
            const uint8_t *data = reinterpret_cast<const uint8_t *>(key.c_str());
            const size_t len = key.length();
            const size_t nblocks = len / 16;

            uint64_t h1 = seed;
            uint64_t h2 = seed;

            // Body
            for (size_t i = 0; i < nblocks; i++)
            {
                uint64_t k1 = *reinterpret_cast<const uint64_t *>(data + i * 16);
                uint64_t k2 = *reinterpret_cast<const uint64_t *>(data + i * 16 + 8);

                k1 *= c1;
                k1 = (k1 << 31) | (k1 >> 33);
                k1 *= c2;
                h1 ^= k1;

                h1 = (h1 << 27) | (h1 >> 37);
                h1 += h2;
                h1 = h1 * 5 + 0x52dce729;

                k2 *= c2;
                k2 = (k2 << 33) | (k2 >> 31);
                k2 *= c1;
                h2 ^= k2;

                h2 = (h2 << 31) | (h2 >> 33);
                h2 += h1;
                h2 = h2 * 5 + 0x38495ab5;
            }

            // Tail
            const uint8_t *tail = data + nblocks * 16;
            uint64_t k1 = 0;
            uint64_t k2 = 0;

            switch (len & 15)
            {
            case 15:
                k2 ^= static_cast<uint64_t>(tail[14]) << 48;
            case 14:
                k2 ^= static_cast<uint64_t>(tail[13]) << 40;
            case 13:
                k2 ^= static_cast<uint64_t>(tail[12]) << 32;
            case 12:
                k2 ^= static_cast<uint64_t>(tail[11]) << 24;
            case 11:
                k2 ^= static_cast<uint64_t>(tail[10]) << 16;
            case 10:
                k2 ^= static_cast<uint64_t>(tail[9]) << 8;
            case 9:
                k2 ^= static_cast<uint64_t>(tail[8]);
                k2 *= c2;
                k2 = (k2 << 33) | (k2 >> 31);
                k2 *= c1;
                h2 ^= k2;
            case 8:
                k1 ^= static_cast<uint64_t>(tail[7]) << 56;
            case 7:
                k1 ^= static_cast<uint64_t>(tail[6]) << 48;
            case 6:
                k1 ^= static_cast<uint64_t>(tail[5]) << 40;
            case 5:
                k1 ^= static_cast<uint64_t>(tail[4]) << 32;
            case 4:
                k1 ^= static_cast<uint64_t>(tail[3]) << 24;
            case 3:
                k1 ^= static_cast<uint64_t>(tail[2]) << 16;
            case 2:
                k1 ^= static_cast<uint64_t>(tail[1]) << 8;
            case 1:
                k1 ^= static_cast<uint64_t>(tail[0]);
                k1 *= c1;
                k1 = (k1 << 31) | (k1 >> 33);
                k1 *= c2;
                h1 ^= k1;
            }

            // Finalization
            h1 ^= len;
            h2 ^= len;

            h1 += h2;
            h2 += h1;

            h1 = fmix64(h1);
            h2 = fmix64(h2);

            h1 += h2;
            h2 += h1;

            return h1;
        }

        uint64_t fmix64(uint64_t k) const
        {
            k ^= k >> 33;
            k *= 0xff51afd7ed558ccdULL;
            k ^= k >> 33;
            k *= 0xc4ceb9fe1a85ec53ULL;
            k ^= k >> 33;
            return k;
        }
    };

    /**
     * PrimaryIndex class that implements the structure shown in the Mermaid diagram:
     * PrimaryIndex --> BBHash
     * BBHash --> Hash_array
     * PrimaryIndex --> value_array
     * PrimaryIndex --> segmentids_list
     */
    template <typename Hasher_t = StringHasher>
    class PrimaryIndex
    {
    private:
        // BBHash component
        boomphf::mphf<Hasher_t> bbhash_;

        std::vector<uint64_t> segmentids_list_;

        // Bit-packed array for value_array_ to save memory
        BitPackedArray value_array_;
        std::vector<uint64_t> hash_array_;
        // Configuration parameters
        double gamma_factor_;
        int num_threads_;
        bool built_;

    public:
        // Constructor
        PrimaryIndex(double gamma = 10.0, int threads = 8)
            : gamma_factor_(gamma), num_threads_(threads), built_(false) {}

        // Destructor
        ~PrimaryIndex() = default;

        // Build the index from segments, where each segment contains multiple keys
        template <typename SegmentContainer>
        void build(const SegmentContainer &segments)
        {
            if (built_)
            {
                throw std::runtime_error("PrimaryIndex already built");
            }

            // Build segmentids_list_ from segment IDs
            segmentids_list_.reserve(segments.size());
            for (const auto &segment : segments)
            {
                segmentids_list_.push_back(segment.segment_id);
            }

            // Collect all keys from all segments
            std::vector<std::string> all_keys;
            std::vector<uint64_t> key_to_segment_index; // Maps key index to segment index

            for (size_t segment_idx = 0; segment_idx < segments.size(); ++segment_idx)
            {
                const auto &segment = segments[segment_idx];
                for (const auto &key : segment.keys)
                {
                    all_keys.push_back(key);
                    key_to_segment_index.push_back(segment_idx);
                }
            }

            // Build the BBHash
            bbhash_ = boomphf::mphf<Hasher_t>(
                all_keys.size(), all_keys, num_threads_, gamma_factor_);

            // Initialize bit-packed array with maximum segment index
            uint64_t max_segment_index = segments.size() - 1;
            value_array_.init(max_segment_index, all_keys.size());

            hash_array_.resize(all_keys.size());

            // Build value_array_ by looking up each key and storing the segment index
            for (size_t i = 0; i < all_keys.size(); ++i)
            {
                auto [idx, hash] = bbhash_.lookup1(all_keys[i]);
                hash_array_[idx] = hash;
                if (idx != std::numeric_limits<uint64_t>::max())
                {
                    // Store the segment index (which is the index in segmentids_list_)
                    value_array_.set(idx, key_to_segment_index[i]);
                }
            }

            built_ = true;
        }

        // Lookup a key and return its segment ID, or -1 if not found
        template <class elem_t>
        int64_t lookup(elem_t key)
        {
            if (!built_)
            {
                return -1;
            }

            std::pair<uint64_t, uint64_t> result = bbhash_.lookup1(key);
            uint64_t idx = result.first;
            if (idx != std::numeric_limits<uint64_t>::max() && idx < value_array_.size() && hash_array_[idx] == result.second)
            {
                // Get the segment index from value_array_
                uint64_t segment_index = value_array_.get(idx);

                // Get the actual segment ID from segmentids_list_
                if (segment_index < segmentids_list_.size())
                {
                    return static_cast<int64_t>(segmentids_list_[segment_index]);
                }
            }

            return -1;
        }

        // Get the BBHash object
        const boomphf::mphf<Hasher_t> *get_bbhash() const
        {
            return &bbhash_;
        }

        void reset_segment_id(uint64_t to_segment_id, uint64_t from_segment_id)
        {
            for (size_t i = 0; i < segmentids_list_.size(); i++)
            {
                if (segmentids_list_[i] == from_segment_id)
                {
                    segmentids_list_[i] = to_segment_id;
                }
            }
        }

        double calculate_segmentid_invalid_percentage() const
        {
            if (segmentids_list_.empty())
            {
                return 0.0;
            }

            uint64_t invalid_count = 0;
            for (size_t i = 0; i < segmentids_list_.size(); i++)
            {
                if (segmentids_list_[i] == -1)
                {
                    invalid_count++;
                }
            }
            return (invalid_count * 100.0) / segmentids_list_.size();
        }

        // Serialization methods for mmap support
        void save_to_file(const std::string &filename) const
        {
            if (!built_)
            {
                throw std::runtime_error("PrimaryIndex not built");
            }

            std::ofstream out(filename, std::ios::binary);
            if (!out)
            {
                throw std::runtime_error("Cannot open file for writing: " + filename);
            }

            // Write header
            uint32_t magic = 0x50494E44; // "PIND"
            out.write(reinterpret_cast<const char *>(&magic), sizeof(magic));

            // Write configuration
            out.write(reinterpret_cast<const char *>(&gamma_factor_), sizeof(gamma_factor_));
            out.write(reinterpret_cast<const char *>(&num_threads_), sizeof(num_threads_));

            // Write BBHash
            bbhash_.save(out);

            // Write segmentids_list_
            uint32_t segment_count = segmentids_list_.size();
            out.write(reinterpret_cast<const char *>(&segment_count), sizeof(segment_count));
            out.write(reinterpret_cast<const char *>(segmentids_list_.data()),
                      segment_count * sizeof(uint64_t));

            // Write hash_array_
            uint32_t hash_count = hash_array_.size();
            out.write(reinterpret_cast<const char *>(&hash_count), sizeof(hash_count));
            out.write(reinterpret_cast<const char *>(hash_array_.data()),
                      hash_count * sizeof(uint64_t));

            // Write value_array_
            value_array_.serialize(out);
        }

        void load_from_file(const std::string &filename)
        {
            std::ifstream in(filename, std::ios::binary);
            if (!in)
            {
                throw std::runtime_error("Cannot open file for reading: " + filename);
            }

            // Read header
            uint32_t magic;
            in.read(reinterpret_cast<char *>(&magic), sizeof(magic));
            if (magic != 0x50494E44)
            {
                throw std::runtime_error("Invalid file format");
            }

            // Read configuration
            in.read(reinterpret_cast<char *>(&gamma_factor_), sizeof(gamma_factor_));
            in.read(reinterpret_cast<char *>(&num_threads_), sizeof(num_threads_));

            // Read BBHash
            try
            {
                bbhash_.load(in);
            }
            catch (const std::exception &e)
            {
                throw std::runtime_error("Failed to load BBHash: " + std::string(e.what()));
            }

            // Read segmentids_list_
            uint32_t segment_count;
            in.read(reinterpret_cast<char *>(&segment_count), sizeof(segment_count));
            segmentids_list_.resize(segment_count);
            in.read(reinterpret_cast<char *>(segmentids_list_.data()),
                    segment_count * sizeof(uint64_t));

            // Read hash_array_
            uint32_t hash_count;
            in.read(reinterpret_cast<char *>(&hash_count), sizeof(hash_count));
            hash_array_.resize(hash_count);
            in.read(reinterpret_cast<char *>(hash_array_.data()),
                    hash_count * sizeof(uint64_t));

            // Read value_array_
            value_array_.deserialize(in);

            built_ = true;
        }

        // MMAP loading: only mmap hash_array_, others use file stream
        bool load_from_mmap(const std::string &filename)
        {
            std::ifstream in(filename, std::ios::binary);
            if (!in)
                return false;

            // Read header
            uint32_t magic;
            in.read(reinterpret_cast<char *>(&magic), sizeof(magic));
            if (magic != 0x50494E44)
                return false;

            // Read configuration
            in.read(reinterpret_cast<char *>(&gamma_factor_), sizeof(gamma_factor_));
            in.read(reinterpret_cast<char *>(&num_threads_), sizeof(num_threads_));

            // Read BBHash
            bbhash_.load(in);

            // Read segmentids_list_
            uint32_t segment_count;
            in.read(reinterpret_cast<char *>(&segment_count), sizeof(segment_count));
            segmentids_list_.resize(segment_count);
            in.read(reinterpret_cast<char *>(segmentids_list_.data()), segment_count * sizeof(uint64_t));

            std::streamoff hash_array_offset = in.tellg();
            uint32_t hash_count;
            in.read(reinterpret_cast<char *>(&hash_count), sizeof(hash_count));

            int fd = open(filename.c_str(), O_RDONLY);
            if (fd == -1)
                return false;
            struct stat st;
            if (fstat(fd, &st) == -1)
            {
                close(fd);
                return false;
            }
            size_t file_size = st.st_size;
            void *mmap_ptr = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (mmap_ptr == MAP_FAILED)
            {
                close(fd);
                return false;
            }
            const char *data = static_cast<const char *>(mmap_ptr);
            size_t hash_array_data_offset = static_cast<size_t>(hash_array_offset) + sizeof(uint32_t);
            hash_array_.resize(hash_count);
            memcpy(hash_array_.data(), data + hash_array_data_offset, hash_count * sizeof(uint64_t));
            munmap(mmap_ptr, file_size);
            close(fd);

            in.seekg(hash_array_offset + sizeof(uint32_t) + hash_count * sizeof(uint64_t));
            value_array_.deserialize(in);

            built_ = true;
            return true;
        }
    };

} // namespace primaryIndex