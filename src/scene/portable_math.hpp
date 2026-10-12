#pragma once

namespace ts2::scene::portable {

// Sine, cosine, logarithm, exponential and power built only from +, -, *, / and exact
// operations (floor, frexp, ldexp), so they give the same bits on every platform and compiler,
// unlike the C library's. Accurate to about a unit in the last place of a double, far finer
// than the floats they feed. Angles in radians; meant for everyday values (|x| up to about
// a million for sine and cosine, still exact-repeatable beyond).
double sine(double x);
double cosine(double x);
double log(double x);   // natural logarithm; NaN below 0, -infinity at 0
double exp(double x);
double power(double x, double y);  // for x >= 0 (NaN for negative x)

} // namespace ts2::scene::portable
