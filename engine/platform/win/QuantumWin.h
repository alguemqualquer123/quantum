// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef QuantumWin_h
#define QuantumWin_h

// Entry point of the Win32 backend for exported games. The window is WindowWin,
// input translation is WinInputRouter and the quantum::System surface is
// SystemWin, so what is left here is the frame loop, the WGL context and the
// build settings.
class QuantumWin {
public:

    static int init(int argc, char **argv);
};

#endif /* QuantumWin_h */
