#pragma once
// 跨进程 Vyukov 队列（定长 POD）
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <unistd.h>

namespace shmring {

constexpr std::uint32_t kMagic    = 0x56444231;  // "VDB1"
constexpr std::uint32_t kVersion  = 1;
constexpr std::size_t   kCacheLine = 64;

// 跨进程共享数据：只能是定长 POD
struct DataPoint {
    std::uint64_t timestamp_ms;
    double        speed;
    double        temp;
};
static_assert(std::is_trivially_copyable_v<DataPoint>);

// 共享内存头部：head/tail 各占一条 cache line
struct Header {
    alignas(kCacheLine) std::atomic<std::uint64_t> head{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> tail{0};
    alignas(kCacheLine) std::uint32_t magic{0};
    std::uint32_t version{0};
    std::uint64_t capacity{0};
    std::uint64_t mask{0};
    std::uint64_t elem_size{0};
};

template <typename T>
struct Slot {
    std::atomic<std::uint64_t> seq{0};
    T data{};
};

template <typename T>
inline std::size_t totalBytes(std::size_t capacity) {
    return sizeof(Header) + capacity * sizeof(Slot<T>);
}

template <typename T>
inline Slot<T>* slotsOf(Header* h) {
    return reinterpret_cast<Slot<T>*>(reinterpret_cast<char*>(h) + sizeof(Header));
}

template <typename T>
class ShmRing {
public:
    // 创建：先 unlink 旧区域，再创建新的
    static ShmRing<T>* create(const char* name, std::size_t capacity) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) return nullptr;

        // 已存在 → 直接打开（不破坏其他进程的映射）
        if (int probe = shm_open(name, O_RDWR, 0666); probe >= 0) {
            ::close(probe);
            return ShmRing<T>::open(name);
        }

        // 不存在 → 创建
        int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
        if (fd < 0) return nullptr;

        std::size_t sz = totalBytes<T>(capacity);
        if (ftruncate(fd, sz) != 0) { ::close(fd); shm_unlink(name); return nullptr; }

        void* p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) { shm_unlink(name); return nullptr; }

        auto* self = new ShmRing<T>();
        self->hdr_   = static_cast<Header*>(p);
        self->base_  = p;
        self->total_ = sz;
        self->name_  = name;
        self->owns_  = true;

        // shared memory 初始为 0；只需填写 header 元数据
        self->hdr_->magic     = kMagic;
        self->hdr_->version   = kVersion;
        self->hdr_->capacity  = capacity;
        self->hdr_->mask      = capacity - 1;
        self->hdr_->elem_size = sizeof(T);

        Slot<T>* slots = slotsOf<T>(self->hdr_);
        for (std::size_t i = 0; i < capacity; ++i) {
            slots[i].seq.store(i, std::memory_order_relaxed);
        }
        return self;
    }

    // 打开已存在的区域
    static ShmRing<T>* open(const char* name) {
        int fd = shm_open(name, O_RDWR, 0666);
        if (fd < 0) return nullptr;

        struct stat st{};
        if (fstat(fd, &st) != 0) { ::close(fd); return nullptr; }

        void* p = mmap(nullptr, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) return nullptr;

        auto* hdr = static_cast<Header*>(p);
        if (hdr->magic != kMagic || hdr->version != kVersion) {
            munmap(p, st.st_size);
            return nullptr;
        }

        auto* self = new ShmRing<T>();
        self->hdr_   = hdr;
        self->base_  = p;
        self->total_ = st.st_size;
        self->name_  = name;
        self->owns_  = false;
        return self;
    }

    static void unlink(const char* name) { shm_unlink(name); }

    bool push(const T& v) {
        Slot<T>* slots = slotsOf<T>(hdr_);
        const std::uint64_t mask = hdr_->mask;
        for (;;) {
            std::uint64_t pos = hdr_->tail.load(std::memory_order_relaxed);
            Slot<T>& s = slots[pos & mask];
            std::uint64_t seq = s.seq.load(std::memory_order_acquire);
            std::intptr_t d = static_cast<std::intptr_t>(seq)
                            - static_cast<std::intptr_t>(pos);
            if (d == 0) {
                if (hdr_->tail.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                    s.data = v;
                    s.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (d < 0) {
                return false;  // 队满
            } else {
                std::this_thread::yield();
            }
        }
    }

    bool pop(T& out) {
        Slot<T>* slots = slotsOf<T>(hdr_);
        const std::uint64_t mask = hdr_->mask;
        const std::uint64_t cap  = hdr_->capacity;
        for (;;) {
            std::uint64_t pos = hdr_->head.load(std::memory_order_relaxed);
            Slot<T>& s = slots[pos & mask];
            std::uint64_t seq = s.seq.load(std::memory_order_acquire);
            std::intptr_t d = static_cast<std::intptr_t>(seq)
                            - static_cast<std::intptr_t>(pos + 1);
            if (d == 0) {
                if (hdr_->head.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                    out = s.data;
                    s.seq.store(pos + cap, std::memory_order_release);
                    return true;
                }
            } else if (d < 0) {
                return false;  // 队空
            } else {
                std::this_thread::yield();
            }
        }
    }

    void close() {
        if (base_ && total_) munmap(base_, total_);
        if (owns_) shm_unlink(name_.c_str());
        delete this;
    }

    Header* header() const { return hdr_; }

private:
    ShmRing() = default;
    Header*     hdr_  = nullptr;
    void*       base_ = nullptr;
    std::size_t total_ = 0;
    std::string name_;
    bool        owns_ = false;
};

}  // namespace shmring
