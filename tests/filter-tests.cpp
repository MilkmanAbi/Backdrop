#include "../filter.cpp"
#include <cstdio>
#include <cmath>
#include <utility>
#include <initializer_list>

int main() {
    int failed = 0;
    // Every minute, including exact boundaries, daytime, overnight and empty windows.
    for (auto range : {std::pair<int,int>{420,1260}, {1260,420}, {420,420}, {0,1439}}) {
        g_fs.from = range.first; g_fs.to = range.second;
        bool active[1440]{};
        for (int minute = range.first; minute != range.second; minute = (minute + 1) % 1440)
            active[minute] = true;
        for (int m = 0; m < 1440; ++m) {
            if (InWindow(m) != active[m]) ++failed;
        }
    }
    for (int type = 0; type < FILTER_COUNT; ++type) {
        float matrix[3][3], offset[3];
        FilterMatrix(type, 0, matrix, offset);
        for (int r = 0; r < 3; ++r) {
            if (std::abs(offset[r]) > 0.00001f) ++failed;
            for (int c = 0; c < 3; ++c)
                if (std::abs(matrix[r][c] - (r == c ? 1.f : 0.f)) > 0.00001f) ++failed;
        }
        FilterMatrix(type, 1, matrix, offset);
        for (auto& row : matrix) for (float value : row) if (!std::isfinite(value)) ++failed;
    }
    printf("schedule: 5760 minute checks; filters: 11 identity/finite checks; failures=%d\n", failed);
    return failed ? 1 : 0;
}
