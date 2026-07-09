#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include <Value.h>

enum class PageType : std::uint8_t {
    Internal = 0,
    Leaf
};

class BTreePage {
    public:
        explicit BTreePage(char *page);
        virtual ~BTreePage() = default;
        static PageType peek_page_type(const char *page);

        bool is_leaf() const;
        std::uint16_t get_key_count();
        std::size_t lower_bound_key(std::uint64_t key) const;
        std::optional<std::uint64_t> first_key() const;
        std::optional<std::uint64_t> key_at(std::size_t idx) const;
        virtual void write_back();
        virtual bool remove(std::uint64_t key) = 0;

    protected:
        virtual void decode() = 0;
        virtual void flush() = 0;

        char *page;
        std::vector<std::uint64_t> keys;
        PageType page_type;
        std::uint16_t free_offset_start;
        std::uint16_t free_offset_end;
        std::uint16_t key_count;
};

class BInternalPage : public BTreePage {
    public:
        explicit BInternalPage(char *page);
        bool insert_separator_at(std::size_t idx, std::uint64_t key, std::uint32_t right_child_page_num);
        bool remove_separator_at(std::size_t idx);
        std::optional<std::uint32_t> get_left_child(std::size_t separator_idx) const;
        std::optional<std::uint32_t> get_right_child(std::size_t separator_idx) const;
        bool remove(std::uint64_t key) override;
        void write_back() override;

    protected:
        void decode() override;
        void flush() override;
        void parse_internal_cell(const char *in, std::uint64_t *key, std::uint32_t *right_child_page_num);

    private:
        // Internal pages hold M separator keys and M + 1 child page numbers.
        std::vector<std::uint32_t> child_page_nums;
};

class BLeafPage : public BTreePage {
    public:
        explicit BLeafPage(char *page);

        std::optional<Value> get(std::uint64_t key) const;
        bool insert_at(std::size_t idx, std::uint64_t key, const Value &value);
        bool remove_at(std::size_t idx);
        bool set(std::uint64_t key, const Value &value);
        bool remove(std::uint64_t key) override;
        void write_back() override;

    protected:
        void decode() override;
        void flush() override;
        void parse_leaf_cell(const char *in, std::uint64_t *key, Value *value);

    private:
        std::vector<Value> values;
};
