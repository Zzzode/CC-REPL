// C++23 Module: Circular buffer
// 固定容量的环形缓冲区模板类，支持迭代器
module;
#include <array>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

export module cc.utils.circular_buffer;

export namespace cc::utils {

// 固定大小的环形缓冲区
template<typename T, size_t N>
class CircularBuffer {
public:
    // 迭代器类型
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        Iterator(const CircularBuffer* buf, size_t pos, size_t count)
            : buf_(buf), pos_(pos), remaining_(count) {}

        reference operator*() const { return buf_->data_[pos_ % N]; }
        pointer operator->() const { return &buf_->data_[pos_ % N]; }

        Iterator& operator++() {
            ++pos_;
            --remaining_;
            return *this;
        }
        Iterator operator++(int) { auto tmp = *this; ++(*this); return tmp; }

        bool operator==(const Iterator& other) const { return remaining_ == other.remaining_; }
        bool operator!=(const Iterator& other) const { return !(*this == other); }

    private:
        const CircularBuffer* buf_;
        size_t pos_;
        size_t remaining_;
    };

    using value_type = T;
    using size_type = size_t;
    using reference = T&;
    using const_reference = const T&;
    using iterator = Iterator;
    using const_iterator = Iterator;

    CircularBuffer() = default;

    // 向尾部添加元素 (满时覆盖最旧元素)
    void push_back(const T& value) {
        data_[tail_] = value;
        advance_tail();
    }

    void push_back(T&& value) {
        data_[tail_] = std::move(value);
        advance_tail();
    }

    // TypeScript-compatible API aliases.
    void add(const T& value) { push_back(value); }
    void add(T&& value) { push_back(std::move(value)); }
    void add_all(const std::vector<T>& values) {
        for (const auto& value : values) push_back(value);
    }

    // 从头部移除元素
    void pop_front() {
        if (empty()) return;
        head_ = (head_ + 1) % N;
        --size_;
    }

    // 访问头部/尾部元素
    [[nodiscard]] reference front() { return data_[head_]; }
    [[nodiscard]] const_reference front() const { return data_[head_]; }
    [[nodiscard]] reference back() {
        return data_[(tail_ + N - 1) % N];
    }
    [[nodiscard]] const_reference back() const {
        return data_[(tail_ + N - 1) % N];
    }

    // 按索引访问 (0 = 最旧)
    [[nodiscard]] reference operator[](size_t index) {
        return data_[(head_ + index) % N];
    }
    [[nodiscard]] const_reference operator[](size_t index) const {
        return data_[(head_ + index) % N];
    }

    // 容量与状态查询
    [[nodiscard]] bool full() const { return size_ == N; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] size_type size() const { return size_; }
    [[nodiscard]] size_type length() const { return size_; }
    [[nodiscard]] static constexpr size_type capacity() { return N; }

    [[nodiscard]] std::vector<T> to_array() const {
        std::vector<T> result;
        result.reserve(size_);
        for (const auto& value : *this) result.push_back(value);
        return result;
    }

    [[nodiscard]] std::vector<T> get_recent(size_t count) const {
        std::vector<T> all = to_array();
        if (count >= all.size()) return all;
        return std::vector<T>(all.end() - static_cast<std::ptrdiff_t>(count), all.end());
    }

    // 清空缓冲区
    void clear() { head_ = tail_ = size_ = 0; }

    // 迭代器支持
    [[nodiscard]] iterator begin() const { return Iterator(this, head_, size_); }
    [[nodiscard]] iterator end() const { return Iterator(this, head_ + size_, 0); }

private:
    std::array<T, N> data_{};
    size_t head_{0};  // 读取位置
    size_t tail_{0};  // 写入位置
    size_t size_{0};  // 当前元素数量

    // 推进写指针
    void advance_tail() {
        if (size_ == N) {
            // 缓冲区满，覆盖最旧数据
            head_ = (head_ + 1) % N;
        } else {
            ++size_;
        }
        tail_ = (tail_ + 1) % N;
    }
};

} // namespace cc::utils
