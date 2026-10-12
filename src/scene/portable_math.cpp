#include "scene/portable_math.hpp"

#include <cmath>
#include <limits>

// The polynomials and constants are the classic ones from Sun's fdlibm (freely usable, see its
// notice: "Developed at SunPro, a Sun Microsystems, Inc. business. Permission to use, copy,
// modify, and distribute this software is freely granted, provided that this notice is
// preserved.").

namespace ts2::scene::portable {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// sin(x) and cos(x) for |x| <= pi/4.
double sineNear(double x) {
    constexpr double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                     S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                     S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
    const double z = x * x;
    const double v = z * x;
    const double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    return x + v * (S1 + z * r);
}

double cosineNear(double x) {
    constexpr double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                     C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                     C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
    const double z = x * x;
    const double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    const double hz = 0.5 * z;
    const double w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + z * r);
}

// x as a quarter turn count and the rest within [-pi/4, pi/4].
double reduce(double x, long long& quarter) {
    constexpr double kTwoOverPi = 6.36619772367581382433e-01;
    // pi/2 in three parts, each short enough that n * part is exact for the n used here.
    constexpr double kPart1 = 1.57079632673412561417e+00, kPart2 = 6.07710050630396597660e-11,
                     kPart3 = 2.02226624871116645580e-21;
    constexpr double kLimit = 1.0e6;
    if (x > kLimit || x < -kLimit) {
        x = std::fmod(x, 6.28318530717958647692);  // exact, so still the same everywhere
    }
    const double n = std::floor(x * kTwoOverPi + 0.5);
    quarter = static_cast<long long>(n);
    return ((x - n * kPart1) - n * kPart2) - n * kPart3;
}

} // namespace

double sine(double x) {
    if (!std::isfinite(x)) {
        return kNaN;
    }
    long long q = 0;
    const double r = reduce(x, q);
    switch (q & 3) {
    case 0: return sineNear(r);
    case 1: return cosineNear(r);
    case 2: return -sineNear(r);
    default: return -cosineNear(r);
    }
}

double cosine(double x) {
    if (!std::isfinite(x)) {
        return kNaN;
    }
    long long q = 0;
    const double r = reduce(x, q);
    switch (q & 3) {
    case 0: return cosineNear(r);
    case 1: return -sineNear(r);
    case 2: return -cosineNear(r);
    default: return sineNear(r);
    }
}

double log(double x) {
    if (std::isnan(x) || x < 0) {
        return kNaN;
    }
    if (x == 0) {
        return -kInf;
    }
    if (x == kInf) {
        return kInf;
    }
    constexpr double kLn2Hi = 6.93147180369123816490e-01, kLn2Lo = 1.90821492927058770002e-10;
    constexpr double Lg1 = 6.666666666666735130e-01, Lg2 = 3.999999999940941908e-01, Lg3 = 2.857142874366239149e-01,
                     Lg4 = 2.222219843214978396e-01, Lg5 = 1.818357216161805012e-01, Lg6 = 1.531383769920937332e-01,
                     Lg7 = 1.479819860511658591e-01;
    int e = 0;
    double m = std::frexp(x, &e);  // x = m * 2^e, m in [0.5, 1)
    if (m < 7.07106781186547524401e-01) {
        m *= 2;
        --e;
    }
    const double k = static_cast<double>(e);
    const double f = m - 1.0;
    const double s = f / (2.0 + f);
    const double z = s * s;
    const double w = z * z;
    const double t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    const double t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    const double r = t2 + t1;
    const double hfsq = 0.5 * f * f;
    return k * kLn2Hi - ((hfsq - (s * (hfsq + r) + k * kLn2Lo)) - f);
}

double exp(double x) {
    if (std::isnan(x)) {
        return kNaN;
    }
    if (x > 7.09782712893383973096e+02) {
        return kInf;
    }
    if (x < -7.45133219101941108420e+02) {
        return 0;
    }
    constexpr double kLn2Hi = 6.93147180369123816490e-01, kLn2Lo = 1.90821492927058770002e-10,
                     kInvLn2 = 1.44269504088896338700e+00;
    constexpr double P1 = 1.66666666666666019037e-01, P2 = -2.77777777770155933842e-03, P3 = 6.61375632143793436117e-05,
                     P4 = -1.65339022054652515390e-06, P5 = 4.13813679705723846039e-08;
    const double k = std::floor(x * kInvLn2 + 0.5);
    const double hi = x - k * kLn2Hi;
    const double lo = k * kLn2Lo;
    const double r = hi - lo;
    const double t = r * r;
    const double c = r - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    const double y = 1.0 - ((lo - (r * c) / (2.0 - c)) - hi);
    return std::ldexp(y, static_cast<int>(k));
}

double power(double x, double y) {
    if (std::isnan(x) || std::isnan(y) || x < 0) {
        return kNaN;
    }
    if (y == 0 || x == 1) {
        return 1;
    }
    if (x == 0) {
        return y > 0 ? 0 : kInf;
    }
    return exp(y * log(x));
}

} // namespace ts2::scene::portable
