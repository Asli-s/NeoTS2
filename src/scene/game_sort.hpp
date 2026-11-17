#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace ts2::scene {

// Sorts as the game's own sort does: an introsort (median of three, then a heap sort past a depth of
// twice the base-2 log of the count) down to runs of 16, then one insertion sort. It is not stable,
// and the game's catalog lists depend on the order it leaves equal items in.
template <class T, class Less>
void gameSort(std::vector<T>& v, Less less) {
    constexpr std::ptrdiff_t kRun = 16;
    const auto median = [&](const T& a, const T& b, const T& c) -> const T& {
        if (less(a, b)) {
            return less(b, c) ? b : (less(a, c) ? c : a);
        }
        return less(a, c) ? a : (less(b, c) ? c : b);
    };
    const auto adjustHeap = [&](std::ptrdiff_t first, std::ptrdiff_t hole, std::ptrdiff_t count, T value) {
        const std::ptrdiff_t top = hole;
        std::ptrdiff_t child = 2 * hole + 2;
        while (child < count) {
            if (less(v[first + child], v[first + child - 1])) {
                --child;
            }
            v[first + hole] = std::move(v[first + child]);
            hole = child;
            child = 2 * (child + 1);
        }
        if (child == count) {
            v[first + hole] = std::move(v[first + child - 1]);
            hole = child - 1;
        }
        std::ptrdiff_t parent = (hole - 1) / 2;
        while (hole > top && less(v[first + parent], value)) {
            v[first + hole] = std::move(v[first + parent]);
            hole = parent;
            parent = (hole - 1) / 2;
        }
        v[first + hole] = std::move(value);
    };
    const auto heapSort = [&](std::ptrdiff_t first, std::ptrdiff_t last) {
        const std::ptrdiff_t count = last - first;
        for (std::ptrdiff_t parent = (count - 2) / 2; count >= 2 && parent >= 0; --parent) {
            adjustHeap(first, parent, count, v[first + parent]);
        }
        while (last - first > 1) {
            --last;
            T value = std::move(v[last]);
            v[last] = std::move(v[first]);
            adjustHeap(first, 0, last - first, std::move(value));
        }
    };
    const auto partition = [&](std::ptrdiff_t first, std::ptrdiff_t last, const T pivot) {
        for (;;) {
            while (less(v[first], pivot)) {
                ++first;
            }
            --last;
            while (less(pivot, v[last])) {
                --last;
            }
            if (first >= last) {
                return first;
            }
            std::swap(v[first], v[last]);
            ++first;
        }
    };
    // The depth limit stops a bad pivot run from recursing more than about 2 log2 n deep.
    const auto loop = [&](auto&& self, std::ptrdiff_t first, std::ptrdiff_t last, int depth) -> void {
        while (last - first > kRun) {
            if (depth == 0) {
                heapSort(first, last);
                return;
            }
            --depth;
            const T pivot = median(v[first], v[first + (last - first) / 2], v[last - 1]);
            const std::ptrdiff_t cut = partition(first, last, pivot);
            self(self, cut, last, depth);
            last = cut;
        }
    };
    const auto n = static_cast<std::ptrdiff_t>(v.size());
    if (n < 2) {
        return;
    }
    int log2 = 0;
    for (std::ptrdiff_t k = n; k > 1; k >>= 1) {
        ++log2;
    }
    loop(loop, 0, n, 2 * log2);
    for (std::ptrdiff_t i = 1; i < n; ++i) {
        T value = std::move(v[i]);
        std::ptrdiff_t j = i;
        if (i < kRun && less(value, v[0])) {
            for (; j > 0; --j) {
                v[j] = std::move(v[j - 1]);
            }
        } else {
            while (j > 0 && less(value, v[j - 1])) {
                v[j] = std::move(v[j - 1]);
                --j;
            }
        }
        v[j] = std::move(value);
    }
}

} // namespace ts2::scene
