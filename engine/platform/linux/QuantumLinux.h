// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef QuantumLinux_h
#define QuantumLinux_h

// Entry point of the X11 backend for exported games. The window is WindowLinux,
// input translation is LinuxInputRouter and the quantum::System surface is
// SystemLinux, so what is left here is the frame loop and the GLX context.
class QuantumLinux {
public:

    static int init(int argc, char **argv);
};

#endif /* QuantumLinux_h */
