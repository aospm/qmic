# QMIC- QMI IDL language definition and code generator

QMIC is a a parser and code generator for a QMI IDL format. It produces
C code which can be used to easily build, encode and decode QMI messages.

QMI is used to communicate with remote processors on Qualcomm platforms such as
the modem and other DSPs.

This fork contains several additions to improve support for nested structs,
dynamic arrays, and other nice UX improvements. It requires that you use
[this fork](https://github.com/aospm/qrtr) of libqrtr.

Both projects have been forked and modified with the goal of supporting the many
more complicated QMI messages required for operation of the modem. This work has
resulted in the creation of [libqril](https://github.com/aospm/libqril) a
high-level library which encompasses event handling, async, service discovery,
and state management. As well as the first (WIP) user
[qrild](https://github.com/aospm/qrild) an open source modem HAL for Android
devices with QRTR/QMI modems such as the OnePlus 6.

Ideally the additions made in this fork should be upstreamed [back to the original](https://github.com/andersson/qmic).