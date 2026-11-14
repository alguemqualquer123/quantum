// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "NodeOps.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace quantum {
namespace graph {

    namespace {

        const double kPi = 3.14159265358979323846;

        inline void binaryFloat(GraphRuntime& runtime, const BoundNode& node,
                                double (*fn)(double, double)) {
            op::outFloat(runtime, node, 0, fn(op::number(runtime, node, 0), op::number(runtime, node, 1)));
        }

        inline double rnd() {
            return static_cast<double>(rand()) / (static_cast<double>(RAND_MAX) + 1.0);
        }

        // asFloat()/asInt() return 0 for a string, so a cast from text has to parse it
        // itself. strtoll/strtod are used instead of std::stoll because the whole build
        // runs with _HAS_EXCEPTIONS=0 and stoll throws on malformed input.
        double numberFromString(const std::string& text) {
            return std::strtod(text.c_str(), nullptr);
        }

        int64_t integerFromString(const std::string& text) {
            return static_cast<int64_t>(std::strtoll(text.c_str(), nullptr, 10));
        }

        Quaternion rotationFromEuler(const Vector3& euler, int order) {
            Quaternion result;
            result.fromEulerAngles(euler.x, euler.y, euler.z, static_cast<RotationOrder>(order & 7));
            return result;
        }

        Vector3 colorOf(const GraphValue& value) {
            return value.asVector3();
        }

        // ---- Math: unary ----
        void mathAbs(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return std::fabs(a); }); }
        void mathNegate(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return -a; }); }
        void mathSign(GraphRuntime& rt, const BoundNode& n) {
            binaryFloat(rt, n, [](double a, double) { return a > 0.0 ? 1.0 : (a < 0.0 ? -1.0 : 0.0); });
        }
        void mathSqrt(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return a < 0.0 ? 0.0 : std::sqrt(a); }); }
        void mathCbrt(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return std::cbrt(a); }); }
        void mathExp(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return std::exp(a); }); }
        void mathLog(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return a <= 0.0 ? 0.0 : std::log(a); }); }
        void mathLog2(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return a <= 0.0 ? 0.0 : std::log2(a); }); }
        void mathFloor(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return std::floor(a); }); }
        void mathCeil(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return std::ceil(a); }); }
        void mathRound(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return std::round(a); }); }
        void mathTrunc(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return std::trunc(a); }); }
        void mathFract(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return a - std::floor(a); }); }
        void mathSin(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return std::sin(a); }); }
        void mathCos(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return std::cos(a); }); }
        void mathTan(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return std::tan(a); }); }
        void mathAsin(GraphRuntime& rt, const BoundNode& n) {
            binaryFloat(rt, n, [](double a, double) {
                const double c = a < -1.0 ? -1.0 : (a > 1.0 ? 1.0 : a);
                return std::asin(c);
            });
        }
        void mathAcos(GraphRuntime& rt, const BoundNode& n) {
            binaryFloat(rt, n, [](double a, double) {
                const double c = a < -1.0 ? -1.0 : (a > 1.0 ? 1.0 : a);
                return std::acos(c);
            });
        }
        void mathAtan(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return std::atan(a); }); }
        void mathDegrees(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return a * 180.0 / kPi; }); }
        void mathRadians(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double) { return a * kPi / 180.0; }); }

        // ---- Math: binary ----
        void mathAdd(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double b) { return a + b; }); }
        void mathSub(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double b) { return a - b; }); }
        void mathMul(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double b) { return a * b; }); }
        void mathDiv(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double b) { return b == 0.0 ? 0.0 : a / b; }); }
        void mathMin(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double b) { return a < b ? a : b; }); }
        void mathMax(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double b) { return a > b ? a : b; }); }
        void mathPow(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double b) { return std::pow(a, b); }); }
        void mathMod(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double b) { return b == 0.0 ? 0.0 : std::fmod(a, b); }); }
        void mathAtan2(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double b) { return std::atan2(a, b); }); }
        void mathAverage(GraphRuntime& rt, const BoundNode& n) { binaryFloat(rt, n, [](double a, double b) { return (a + b) * 0.5; }); }

        // ---- Math: three or more inputs ----
        void mathLerp(GraphRuntime& rt, const BoundNode& n) {
            const double a = op::number(rt, n, 0);
            const double b = op::number(rt, n, 1);
            const double t = op::number(rt, n, 2);
            op::outFloat(rt, n, 0, a + (b - a) * t);
        }

        void mathClamp(GraphRuntime& rt, const BoundNode& n) {
            const double v = op::number(rt, n, 0);
            const double lo = op::number(rt, n, 1);
            const double hi = op::number(rt, n, 2);
            op::outFloat(rt, n, 0, v < lo ? lo : (v > hi ? hi : v));
        }

        void mathNormalize(GraphRuntime& rt, const BoundNode& n) {
            const double v = op::number(rt, n, 0);
            const double range = op::number(rt, n, 2) - op::number(rt, n, 1);
            op::outFloat(rt, n, 0, range == 0.0 ? 0.0 : (v - op::number(rt, n, 1)) / range);
        }

        void mathRemap(GraphRuntime& rt, const BoundNode& n) {
            const double v = op::number(rt, n, 0);
            const double inMin = op::number(rt, n, 1);
            const double inMax = op::number(rt, n, 2);
            const double outMin = op::number(rt, n, 3);
            const double outMax = op::number(rt, n, 4);
            const double span = inMax - inMin;
            op::outFloat(rt, n, 0, span == 0.0 ? outMin : outMin + (v - inMin) * (outMax - outMin) / span);
        }

        void mathSmoothstep(GraphRuntime& rt, const BoundNode& n) {
            const double e0 = op::number(rt, n, 0);
            const double e1 = op::number(rt, n, 1);
            const double v = op::number(rt, n, 2);
            const double span = e1 - e0;
            double t = span == 0.0 ? 0.0 : (v - e0) / span;
            t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
            op::outFloat(rt, n, 0, t * t * (3.0 - 2.0 * t));
        }

        void mathPingpong(GraphRuntime& rt, const BoundNode& n) {
            const double v = op::number(rt, n, 0);
            const double lo = op::number(rt, n, 1);
            const double hi = op::number(rt, n, 2);
            const double range = hi - lo;
            if (range <= 0.0) {
                op::outFloat(rt, n, 0, lo);
                return;
            }
            double t = std::fmod(v - lo, 2.0 * range);
            if (t < 0.0) {
                t += 2.0 * range;
            }
            op::outFloat(rt, n, 0, range - std::fabs(t - range) + lo);
        }

        void mathWrap(GraphRuntime& rt, const BoundNode& n) {
            const double v = op::number(rt, n, 0);
            const double lo = op::number(rt, n, 1);
            const double hi = op::number(rt, n, 2);
            const double range = hi - lo;
            if (range <= 0.0) {
                op::outFloat(rt, n, 0, lo);
                return;
            }
            const double offset = v - lo;
            op::outFloat(rt, n, 0, lo + offset - range * std::floor(offset / range));
        }

        void mathRandom(GraphRuntime& rt, const BoundNode& n) {
            const double lo = op::number(rt, n, 0);
            const double hi = op::number(rt, n, 1);
            op::outFloat(rt, n, 0, lo + (hi - lo) * rnd());
        }

        void mathRandomInt(GraphRuntime& rt, const BoundNode& n) {
            int64_t lo = op::integer(rt, n, 0);
            int64_t hi = op::integer(rt, n, 1);
            if (hi < lo) {
                const int64_t t = lo;
                lo = hi;
                hi = t;
            }
            const double span = static_cast<double>(hi - lo) + 1.0;
            int64_t value = lo + static_cast<int64_t>(rnd() * span);
            if (value > hi) {
                value = hi;      // rnd() is < 1, this only guards rounding
            }
            op::outInt(rt, n, 0, value);
        }

        void mathRandomBool(GraphRuntime& rt, const BoundNode& n) {
            op::outBool(rt, n, 0, rnd() < op::number(rt, n, 0));
        }

        // ---- Vector2 ----
        void vector2Make(GraphRuntime& rt, const BoundNode& n) {
            op::outVector2(rt, n, 0, Vector2(static_cast<float>(op::number(rt, n, 0)),
                                             static_cast<float>(op::number(rt, n, 1))));
        }
        void vector2Split(GraphRuntime& rt, const BoundNode& n) {
            const Vector2 v = op::vector2(rt, n, 0);
            op::outFloat(rt, n, 0, v.x);
            op::outFloat(rt, n, 1, v.y);
        }
        void vector2Add(GraphRuntime& rt, const BoundNode& n) { op::outVector2(rt, n, 0, op::vector2(rt, n, 0) + op::vector2(rt, n, 1)); }
        void vector2Sub(GraphRuntime& rt, const BoundNode& n) { op::outVector2(rt, n, 0, op::vector2(rt, n, 0) - op::vector2(rt, n, 1)); }
        void vector2Scale(GraphRuntime& rt, const BoundNode& n) { op::outVector2(rt, n, 0, op::vector2(rt, n, 0) * static_cast<float>(op::number(rt, n, 1))); }
        void vector2Dot(GraphRuntime& rt, const BoundNode& n) { op::outFloat(rt, n, 0, op::vector2(rt, n, 0).dotProduct(op::vector2(rt, n, 1))); }
        void vector2Length(GraphRuntime& rt, const BoundNode& n) { op::outFloat(rt, n, 0, op::vector2(rt, n, 0).length()); }
        void vector2Distance(GraphRuntime& rt, const BoundNode& n) { op::outFloat(rt, n, 0, op::vector2(rt, n, 0).distance(op::vector2(rt, n, 1))); }
        void vector2Normalized(GraphRuntime& rt, const BoundNode& n) { op::outVector2(rt, n, 0, op::vector2(rt, n, 0).normalized()); }
        void vector2Lerp(GraphRuntime& rt, const BoundNode& n) {
            op::outVector2(rt, n, 0, op::vector2(rt, n, 0).lerp(op::vector2(rt, n, 1), static_cast<float>(op::number(rt, n, 2))));
        }

        // ---- Vector3 ----
        void vector3Make(GraphRuntime& rt, const BoundNode& n) {
            op::outVector3(rt, n, 0, Vector3(static_cast<float>(op::number(rt, n, 0)),
                                             static_cast<float>(op::number(rt, n, 1)),
                                             static_cast<float>(op::number(rt, n, 2))));
        }
        void vector3Split(GraphRuntime& rt, const BoundNode& n) {
            const Vector3 v = op::vector3(rt, n, 0);
            op::outFloat(rt, n, 0, v.x);
            op::outFloat(rt, n, 1, v.y);
            op::outFloat(rt, n, 2, v.z);
        }
        void vector3Add(GraphRuntime& rt, const BoundNode& n) { op::outVector3(rt, n, 0, op::vector3(rt, n, 0) + op::vector3(rt, n, 1)); }
        void vector3Sub(GraphRuntime& rt, const BoundNode& n) { op::outVector3(rt, n, 0, op::vector3(rt, n, 0) - op::vector3(rt, n, 1)); }
        void vector3Scale(GraphRuntime& rt, const BoundNode& n) { op::outVector3(rt, n, 0, op::vector3(rt, n, 0) * static_cast<float>(op::number(rt, n, 1))); }
        void vector3Negate(GraphRuntime& rt, const BoundNode& n) { op::outVector3(rt, n, 0, -op::vector3(rt, n, 0)); }
        void vector3Min(GraphRuntime& rt, const BoundNode& n) {
            Vector3 out = op::vector3(rt, n, 0);
            out.makeFloor(op::vector3(rt, n, 1));
            op::outVector3(rt, n, 0, out);
        }
        void vector3Max(GraphRuntime& rt, const BoundNode& n) {
            Vector3 out = op::vector3(rt, n, 0);
            out.makeCeil(op::vector3(rt, n, 1));
            op::outVector3(rt, n, 0, out);
        }
        void vector3Dot(GraphRuntime& rt, const BoundNode& n) { op::outFloat(rt, n, 0, op::vector3(rt, n, 0).dotProduct(op::vector3(rt, n, 1))); }
        void vector3Cross(GraphRuntime& rt, const BoundNode& n) { op::outVector3(rt, n, 0, op::vector3(rt, n, 0).crossProduct(op::vector3(rt, n, 1))); }
        void vector3Length(GraphRuntime& rt, const BoundNode& n) { op::outFloat(rt, n, 0, op::vector3(rt, n, 0).length()); }
        void vector3SquaredLength(GraphRuntime& rt, const BoundNode& n) { op::outFloat(rt, n, 0, op::vector3(rt, n, 0).squaredLength()); }
        void vector3Distance(GraphRuntime& rt, const BoundNode& n) { op::outFloat(rt, n, 0, op::vector3(rt, n, 0).distance(op::vector3(rt, n, 1))); }
        void vector3Normalized(GraphRuntime& rt, const BoundNode& n) { op::outVector3(rt, n, 0, op::vector3(rt, n, 0).normalized()); }
        void vector3Lerp(GraphRuntime& rt, const BoundNode& n) {
            op::outVector3(rt, n, 0, op::vector3(rt, n, 0).lerp(op::vector3(rt, n, 1), static_cast<float>(op::number(rt, n, 2))));
        }
        void vector3MoveTowards(GraphRuntime& rt, const BoundNode& n) {
            op::outVector3(rt, n, 0, op::vector3(rt, n, 0).moveTowards(op::vector3(rt, n, 1), static_cast<float>(op::number(rt, n, 2))));
        }
        void vector3Reflect(GraphRuntime& rt, const BoundNode& n) {
            op::outVector3(rt, n, 0, op::vector3(rt, n, 0).reflect(op::vector3(rt, n, 1)));
        }
        void vector3Angle(GraphRuntime& rt, const BoundNode& n) {
            const Vector3 a = op::vector3(rt, n, 0);
            const Vector3 b = op::vector3(rt, n, 1);
            const double denom = static_cast<double>(a.length()) * static_cast<double>(b.length());
            if (denom <= 0.0) {
                op::outFloat(rt, n, 0, 0.0);
                return;
            }
            double c = static_cast<double>(a.dotProduct(b)) / denom;
            c = c < -1.0 ? -1.0 : (c > 1.0 ? 1.0 : c);
            op::outFloat(rt, n, 0, std::acos(c));
        }
        void vector3Rotate(GraphRuntime& rt, const BoundNode& n) {
            op::outVector3(rt, n, 0, op::quaternion(rt, n, 0) * op::vector3(rt, n, 1));
        }

        // ---- Vector4 ----
        void vector4Make(GraphRuntime& rt, const BoundNode& n) {
            op::outVector4(rt, n, 0, Vector4(static_cast<float>(op::number(rt, n, 0)),
                                             static_cast<float>(op::number(rt, n, 1)),
                                             static_cast<float>(op::number(rt, n, 2)),
                                             static_cast<float>(op::number(rt, n, 3))));
        }
        void vector4Split(GraphRuntime& rt, const BoundNode& n) {
            const Vector4 v = op::vector4(rt, n, 0);
            op::outFloat(rt, n, 0, v.x);
            op::outFloat(rt, n, 1, v.y);
            op::outFloat(rt, n, 2, v.z);
            op::outFloat(rt, n, 3, v.w);
        }
        void vector4Lerp(GraphRuntime& rt, const BoundNode& n) {
            op::outVector4(rt, n, 0, op::vector4(rt, n, 0).lerp(op::vector4(rt, n, 1), static_cast<float>(op::number(rt, n, 2))));
        }

        // ---- Quaternion ----
        void quaternionIdentity(GraphRuntime& rt, const BoundNode& n) { op::outQuaternion(rt, n, 0, Quaternion::IDENTITY); }
        void quaternionMake(GraphRuntime& rt, const BoundNode& n) {
            op::outQuaternion(rt, n, 0, Quaternion(static_cast<float>(op::number(rt, n, 0)),
                                                   static_cast<float>(op::number(rt, n, 1)),
                                                   static_cast<float>(op::number(rt, n, 2)),
                                                   static_cast<float>(op::number(rt, n, 3))));
        }
        void quaternionFromEuler(GraphRuntime& rt, const BoundNode& n) {
            op::outQuaternion(rt, n, 0, rotationFromEuler(op::vector3(rt, n, 0), static_cast<int>(op::integer(rt, n, 1))));
        }
        void quaternionToEuler(GraphRuntime& rt, const BoundNode& n) {
            op::outVector3(rt, n, 0, op::quaternion(rt, n, 0).getEulerAngles(
                static_cast<RotationOrder>(op::integer(rt, n, 1) & 7)));
        }
        void quaternionMultiply(GraphRuntime& rt, const BoundNode& n) {
            op::outQuaternion(rt, n, 0, op::quaternion(rt, n, 0) * op::quaternion(rt, n, 1));
        }
        void quaternionInverse(GraphRuntime& rt, const BoundNode& n) { op::outQuaternion(rt, n, 0, op::quaternion(rt, n, 0).inverse()); }
        void quaternionSlerp(GraphRuntime& rt, const BoundNode& n) {
            op::outQuaternion(rt, n, 0, Quaternion::slerp(static_cast<float>(op::number(rt, n, 2)),
                                                          op::quaternion(rt, n, 0), op::quaternion(rt, n, 1)));
        }
        void quaternionLookRotation(GraphRuntime& rt, const BoundNode& n) {
            op::outQuaternion(rt, n, 0, Quaternion::lookRotation(op::vector3(rt, n, 0), op::vector3(rt, n, 1)));
        }
        void quaternionRotateVector(GraphRuntime& rt, const BoundNode& n) {
            op::outVector3(rt, n, 0, op::quaternion(rt, n, 0) * op::vector3(rt, n, 1));
        }

        // ---- Rect ----
        void rectMake(GraphRuntime& rt, const BoundNode& n) {
            op::outRect(rt, n, 0, Rect(static_cast<float>(op::number(rt, n, 0)), static_cast<float>(op::number(rt, n, 1)),
                                       static_cast<float>(op::number(rt, n, 2)), static_cast<float>(op::number(rt, n, 3))));
        }
        void rectSplit(GraphRuntime& rt, const BoundNode& n) {
            const Rect r = op::rect(rt, n, 0);
            op::outFloat(rt, n, 0, r.getX());
            op::outFloat(rt, n, 1, r.getY());
            op::outFloat(rt, n, 2, r.getWidth());
            op::outFloat(rt, n, 3, r.getHeight());
        }
        void rectContains(GraphRuntime& rt, const BoundNode& n) {
            // Rect::contains is not const, so the pin value is copied before reading it.
            Rect value = op::rect(rt, n, 0);
            op::outBool(rt, n, 0, value.contains(op::vector2(rt, n, 1)));
        }

        // ---- Color ----
        void color3Make(GraphRuntime& rt, const BoundNode& n) {
            op::outColor3(rt, n, 0, Vector3(static_cast<float>(op::number(rt, n, 0)),
                                            static_cast<float>(op::number(rt, n, 1)),
                                            static_cast<float>(op::number(rt, n, 2))));
        }
        void color3Split(GraphRuntime& rt, const BoundNode& n) {
            const Vector3 c = colorOf(op::in(rt, n, 0));
            op::outFloat(rt, n, 0, c.x);
            op::outFloat(rt, n, 1, c.y);
            op::outFloat(rt, n, 2, c.z);
        }
        void color4Make(GraphRuntime& rt, const BoundNode& n) {
            op::outColor4(rt, n, 0, Vector4(static_cast<float>(op::number(rt, n, 0)),
                                            static_cast<float>(op::number(rt, n, 1)),
                                            static_cast<float>(op::number(rt, n, 2)),
                                            static_cast<float>(op::number(rt, n, 3))));
        }
        void color4WithAlpha(GraphRuntime& rt, const BoundNode& n) {
            const Vector3 c = colorOf(op::in(rt, n, 0));
            op::outColor4(rt, n, 0, Vector4(c.x, c.y, c.z, static_cast<float>(op::number(rt, n, 1))));
        }

        int hexDigit(char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        // Accepts "#rgb", "#rrggbb" and "#rrggbbaa"; anything else is opaque white,
        // because a wrong hex string must not black out a whole UI.
        void colorFromHex(GraphRuntime& rt, const BoundNode& n) {
            std::string text = op::text(rt, n, 0);
            if (!text.empty() && text[0] == '#') {
                text.erase(0, 1);
            }

            auto readByte = [&text](size_t pos, size_t digits) -> int {
                int value = 0;
                for (size_t i = 0; i < digits; ++i) {
                    const int d = hexDigit(text[pos + i]);
                    if (d < 0) return -1;
                    value = value * 16 + d;
                }
                return digits == 1 ? value * 17 : value;
            };

            const size_t len = text.size();
            const size_t digits = (len == 3 || len == 4) ? 1 : 2;
            if (len != 3 && len != 4 && len != 6 && len != 8) {
                op::outColor4(rt, n, 0, Vector4(1, 1, 1, 1));
                return;
            }

            const int r = readByte(0, digits);
            const int g = readByte(digits, digits);
            const int b = readByte(digits * 2, digits);
            const int a = len == 3 ? 255 : readByte(digits * 3, digits);
            if (r < 0 || g < 0 || b < 0 || a < 0) {
                op::outColor4(rt, n, 0, Vector4(1, 1, 1, 1));
                return;
            }
            op::outColor4(rt, n, 0, Vector4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f));
        }

        void colorToHex(GraphRuntime& rt, const BoundNode& n) {
            const Vector4 c = op::in(rt, n, 0).asVector4();
            auto byte = [](float v) -> int {
                const int i = static_cast<int>(std::lround((v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v)) * 255.0f));
                return i;
            };
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", byte(c.x), byte(c.y), byte(c.z), byte(c.w));
            op::outString(rt, n, 0, buffer);
        }

        // ---- Casts ----
        void castToBool(GraphRuntime& rt, const BoundNode& n) {
            const GraphValue v = op::in(rt, n, 0);
            if (v.type == GraphValueType::String) {
                const std::string text = v.asString();
                op::outBool(rt, n, 0, text == "true" || (text != "false" && numberFromString(text) != 0.0));
                return;
            }
            op::outBool(rt, n, 0, v.asBool());
        }
        void castToInt(GraphRuntime& rt, const BoundNode& n) {
            const GraphValue v = op::in(rt, n, 0);
            op::outInt(rt, n, 0, v.type == GraphValueType::String ? integerFromString(v.asString()) : v.asInt());
        }
        void castToFloat(GraphRuntime& rt, const BoundNode& n) {
            const GraphValue v = op::in(rt, n, 0);
            op::outFloat(rt, n, 0, v.type == GraphValueType::String ? numberFromString(v.asString()) : v.asFloat());
        }
        void castToString(GraphRuntime& rt, const BoundNode& n) {
            const GraphValue v = op::in(rt, n, 0);
            op::outString(rt, n, 0, v.type == GraphValueType::String ? v.asString() : v.toDisplayString());
        }
        void castToVector2(GraphRuntime& rt, const BoundNode& n) {
            const GraphValue v = op::in(rt, n, 0);
            if (v.type == GraphValueType::Bool || v.type == GraphValueType::Int || v.type == GraphValueType::Float) {
                const float f = static_cast<float>(v.asFloat());
                op::outVector2(rt, n, 0, Vector2(f, f));
                return;
            }
            op::outVector2(rt, n, 0, v.asVector2());
        }
        void castToVector3(GraphRuntime& rt, const BoundNode& n) {
            const GraphValue v = op::in(rt, n, 0);
            if (v.type == GraphValueType::Bool || v.type == GraphValueType::Int || v.type == GraphValueType::Float) {
                const float f = static_cast<float>(v.asFloat());
                op::outVector3(rt, n, 0, Vector3(f, f, f));
                return;
            }
            op::outVector3(rt, n, 0, v.asVector3());
        }
        void castToVector4(GraphRuntime& rt, const BoundNode& n) {
            const GraphValue v = op::in(rt, n, 0);
            if (v.type == GraphValueType::Bool || v.type == GraphValueType::Int || v.type == GraphValueType::Float) {
                const float f = static_cast<float>(v.asFloat());
                op::outVector4(rt, n, 0, Vector4(f, f, f, 1.0f));
                return;
            }
            op::outVector4(rt, n, 0, v.asVector4());
        }
        void castToColor3(GraphRuntime& rt, const BoundNode& n) { op::outColor3(rt, n, 0, op::in(rt, n, 0).asVector3()); }
        void castToColor4(GraphRuntime& rt, const BoundNode& n) { op::outColor4(rt, n, 0, op::in(rt, n, 0).asVector4()); }
        void castToQuaternion(GraphRuntime& rt, const BoundNode& n) {
            const GraphValue v = op::in(rt, n, 0);
            if (v.type == GraphValueType::Vector3) {
                op::outQuaternion(rt, n, 0, rotationFromEuler(v.asVector3(), 0));
                return;
            }
            op::outQuaternion(rt, n, 0, v.asQuaternion());
        }
        void castToEntity(GraphRuntime& rt, const BoundNode& n) {
            const GraphValue v = op::in(rt, n, 0);
            if (v.type == GraphValueType::String) {
                op::outEntity(rt, n, 0, static_cast<Entity>(integerFromString(v.asString())));
                return;
            }
            op::outEntity(rt, n, 0, v.type == GraphValueType::Entity ? v.asEntity()
                                                                     : static_cast<Entity>(v.asInt()));
        }

    }

    void registerMathOps() {
        registerGraphOp("math.add", BoundOpKind::Pure, nullptr, &mathAdd);
        registerGraphOp("math.sub", BoundOpKind::Pure, nullptr, &mathSub);
        registerGraphOp("math.mul", BoundOpKind::Pure, nullptr, &mathMul);
        registerGraphOp("math.div", BoundOpKind::Pure, nullptr, &mathDiv);
        registerGraphOp("math.min", BoundOpKind::Pure, nullptr, &mathMin);
        registerGraphOp("math.max", BoundOpKind::Pure, nullptr, &mathMax);
        registerGraphOp("math.abs", BoundOpKind::Pure, nullptr, &mathAbs);
        registerGraphOp("math.negate", BoundOpKind::Pure, nullptr, &mathNegate);
        registerGraphOp("math.sign", BoundOpKind::Pure, nullptr, &mathSign);
        registerGraphOp("math.sqrt", BoundOpKind::Pure, nullptr, &mathSqrt);
        registerGraphOp("math.cbrt", BoundOpKind::Pure, nullptr, &mathCbrt);
        registerGraphOp("math.exp", BoundOpKind::Pure, nullptr, &mathExp);
        registerGraphOp("math.log", BoundOpKind::Pure, nullptr, &mathLog);
        registerGraphOp("math.log2", BoundOpKind::Pure, nullptr, &mathLog2);
        registerGraphOp("math.floor", BoundOpKind::Pure, nullptr, &mathFloor);
        registerGraphOp("math.ceil", BoundOpKind::Pure, nullptr, &mathCeil);
        registerGraphOp("math.round", BoundOpKind::Pure, nullptr, &mathRound);
        registerGraphOp("math.trunc", BoundOpKind::Pure, nullptr, &mathTrunc);
        registerGraphOp("math.fract", BoundOpKind::Pure, nullptr, &mathFract);
        registerGraphOp("math.sin", BoundOpKind::Pure, nullptr, &mathSin);
        registerGraphOp("math.cos", BoundOpKind::Pure, nullptr, &mathCos);
        registerGraphOp("math.tan", BoundOpKind::Pure, nullptr, &mathTan);
        registerGraphOp("math.asin", BoundOpKind::Pure, nullptr, &mathAsin);
        registerGraphOp("math.acos", BoundOpKind::Pure, nullptr, &mathAcos);
        registerGraphOp("math.atan", BoundOpKind::Pure, nullptr, &mathAtan);
        registerGraphOp("math.degrees", BoundOpKind::Pure, nullptr, &mathDegrees);
        registerGraphOp("math.radians", BoundOpKind::Pure, nullptr, &mathRadians);
        registerGraphOp("math.pow", BoundOpKind::Pure, nullptr, &mathPow);
        registerGraphOp("math.mod", BoundOpKind::Pure, nullptr, &mathMod);
        registerGraphOp("math.atan2", BoundOpKind::Pure, nullptr, &mathAtan2);
        registerGraphOp("math.average", BoundOpKind::Pure, nullptr, &mathAverage);
        registerGraphOp("math.lerp", BoundOpKind::Pure, nullptr, &mathLerp);
        registerGraphOp("math.clamp", BoundOpKind::Pure, nullptr, &mathClamp);
        registerGraphOp("math.normalize", BoundOpKind::Pure, nullptr, &mathNormalize);
        registerGraphOp("math.remap", BoundOpKind::Pure, nullptr, &mathRemap);
        registerGraphOp("math.smoothstep", BoundOpKind::Pure, nullptr, &mathSmoothstep);
        registerGraphOp("math.pingpong", BoundOpKind::Pure, nullptr, &mathPingpong);
        registerGraphOp("math.wrap", BoundOpKind::Pure, nullptr, &mathWrap);
        registerGraphOp("math.random", BoundOpKind::Pure, nullptr, &mathRandom);
        registerGraphOp("math.randomInt", BoundOpKind::Pure, nullptr, &mathRandomInt);
        registerGraphOp("math.randomBool", BoundOpKind::Pure, nullptr, &mathRandomBool);

        registerGraphOp("vector2.make", BoundOpKind::Pure, nullptr, &vector2Make);
        registerGraphOp("vector2.split", BoundOpKind::Pure, nullptr, &vector2Split);
        registerGraphOp("vector2.add", BoundOpKind::Pure, nullptr, &vector2Add);
        registerGraphOp("vector2.sub", BoundOpKind::Pure, nullptr, &vector2Sub);
        registerGraphOp("vector2.scale", BoundOpKind::Pure, nullptr, &vector2Scale);
        registerGraphOp("vector2.dot", BoundOpKind::Pure, nullptr, &vector2Dot);
        registerGraphOp("vector2.length", BoundOpKind::Pure, nullptr, &vector2Length);
        registerGraphOp("vector2.distance", BoundOpKind::Pure, nullptr, &vector2Distance);
        registerGraphOp("vector2.normalized", BoundOpKind::Pure, nullptr, &vector2Normalized);
        registerGraphOp("vector2.lerp", BoundOpKind::Pure, nullptr, &vector2Lerp);

        registerGraphOp("vector3.make", BoundOpKind::Pure, nullptr, &vector3Make);
        registerGraphOp("vector3.split", BoundOpKind::Pure, nullptr, &vector3Split);
        registerGraphOp("vector3.add", BoundOpKind::Pure, nullptr, &vector3Add);
        registerGraphOp("vector3.sub", BoundOpKind::Pure, nullptr, &vector3Sub);
        registerGraphOp("vector3.scale", BoundOpKind::Pure, nullptr, &vector3Scale);
        registerGraphOp("vector3.negate", BoundOpKind::Pure, nullptr, &vector3Negate);
        registerGraphOp("vector3.min", BoundOpKind::Pure, nullptr, &vector3Min);
        registerGraphOp("vector3.max", BoundOpKind::Pure, nullptr, &vector3Max);
        registerGraphOp("vector3.dot", BoundOpKind::Pure, nullptr, &vector3Dot);
        registerGraphOp("vector3.cross", BoundOpKind::Pure, nullptr, &vector3Cross);
        registerGraphOp("vector3.length", BoundOpKind::Pure, nullptr, &vector3Length);
        registerGraphOp("vector3.squaredLength", BoundOpKind::Pure, nullptr, &vector3SquaredLength);
        registerGraphOp("vector3.distance", BoundOpKind::Pure, nullptr, &vector3Distance);
        registerGraphOp("vector3.normalized", BoundOpKind::Pure, nullptr, &vector3Normalized);
        registerGraphOp("vector3.lerp", BoundOpKind::Pure, nullptr, &vector3Lerp);
        registerGraphOp("vector3.moveTowards", BoundOpKind::Pure, nullptr, &vector3MoveTowards);
        registerGraphOp("vector3.reflect", BoundOpKind::Pure, nullptr, &vector3Reflect);
        registerGraphOp("vector3.angle", BoundOpKind::Pure, nullptr, &vector3Angle);
        registerGraphOp("vector3.rotate", BoundOpKind::Pure, nullptr, &vector3Rotate);

        registerGraphOp("vector4.make", BoundOpKind::Pure, nullptr, &vector4Make);
        registerGraphOp("vector4.split", BoundOpKind::Pure, nullptr, &vector4Split);
        registerGraphOp("vector4.lerp", BoundOpKind::Pure, nullptr, &vector4Lerp);

        registerGraphOp("quaternion.identity", BoundOpKind::Pure, nullptr, &quaternionIdentity);
        registerGraphOp("quaternion.make", BoundOpKind::Pure, nullptr, &quaternionMake);
        registerGraphOp("quaternion.fromEuler", BoundOpKind::Pure, nullptr, &quaternionFromEuler);
        registerGraphOp("quaternion.toEuler", BoundOpKind::Pure, nullptr, &quaternionToEuler);
        registerGraphOp("quaternion.multiply", BoundOpKind::Pure, nullptr, &quaternionMultiply);
        registerGraphOp("quaternion.inverse", BoundOpKind::Pure, nullptr, &quaternionInverse);
        registerGraphOp("quaternion.slerp", BoundOpKind::Pure, nullptr, &quaternionSlerp);
        registerGraphOp("quaternion.lookRotation", BoundOpKind::Pure, nullptr, &quaternionLookRotation);
        registerGraphOp("quaternion.rotateVector", BoundOpKind::Pure, nullptr, &quaternionRotateVector);

        registerGraphOp("rect.make", BoundOpKind::Pure, nullptr, &rectMake);
        registerGraphOp("rect.split", BoundOpKind::Pure, nullptr, &rectSplit);
        registerGraphOp("rect.contains", BoundOpKind::Pure, nullptr, &rectContains);

        registerGraphOp("color3.make", BoundOpKind::Pure, nullptr, &color3Make);
        registerGraphOp("color3.split", BoundOpKind::Pure, nullptr, &color3Split);
        registerGraphOp("color4.make", BoundOpKind::Pure, nullptr, &color4Make);
        registerGraphOp("color4.withAlpha", BoundOpKind::Pure, nullptr, &color4WithAlpha);
        registerGraphOp("color.fromHex", BoundOpKind::Pure, nullptr, &colorFromHex);
        registerGraphOp("color.toHex", BoundOpKind::Pure, nullptr, &colorToHex);

        registerGraphOp("cast.toBool", BoundOpKind::Pure, nullptr, &castToBool);
        registerGraphOp("cast.toInt", BoundOpKind::Pure, nullptr, &castToInt);
        registerGraphOp("cast.toFloat", BoundOpKind::Pure, nullptr, &castToFloat);
        registerGraphOp("cast.toString", BoundOpKind::Pure, nullptr, &castToString);
        registerGraphOp("cast.toVector2", BoundOpKind::Pure, nullptr, &castToVector2);
        registerGraphOp("cast.toVector3", BoundOpKind::Pure, nullptr, &castToVector3);
        registerGraphOp("cast.toVector4", BoundOpKind::Pure, nullptr, &castToVector4);
        registerGraphOp("cast.toColor3", BoundOpKind::Pure, nullptr, &castToColor3);
        registerGraphOp("cast.toColor4", BoundOpKind::Pure, nullptr, &castToColor4);
        registerGraphOp("cast.toQuaternion", BoundOpKind::Pure, nullptr, &castToQuaternion);
        registerGraphOp("cast.toEntity", BoundOpKind::Pure, nullptr, &castToEntity);
    }

}
}
