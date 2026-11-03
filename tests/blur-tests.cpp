#include "../blur.cpp"
#include <cstdio>

int main() {
    int failures = 0;
    for (int i = 0; i <= 100; ++i) {
        Pass pass{};
        Kernel(i / 10.f, &pass);
        float total = pass.tap[0][0];
        if (pass.taps < 1 || pass.taps > 12) ++failures;
        for (int j = 1; j < pass.taps; ++j) {
            total += 2 * pass.tap[j][0];
            if (!std::isfinite(pass.tap[j][1]) || pass.tap[j][0] < 0) ++failures;
        }
        if (std::abs(total - 1.f) > 0.0001f) ++failures;
    }
    float fix[3][4];
    ColourFix(fix);
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c)
        if (std::abs(fix[r][c] - (r == c ? 1.f : 0.f)) > 0.0001f) ++failures;
    printf("blur: 101 normalized finite kernels and identity colour correction; failures=%d\n", failures);
    return failures ? 1 : 0;
}
