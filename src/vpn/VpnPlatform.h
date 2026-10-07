//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef VPNPLATFORM_H
#define VPNPLATFORM_H

#include <QtGlobal>

//! Does this build have a native VPN integration at all?
//!
//! Linux drives a privileged helper script, Windows drives the WireGuard
//! tunnel service and the OpenVPN interactive service. macOS has neither, so
//! every VPN affordance -- the settings button, the state label, the
//! per-server "Use VPN" column, and the job admission rules that can block a
//! post -- must be inert there rather than merely failing late with a message
//! about installing a helper that does not exist for that platform.
//!
//! This is deliberately one macro rather than a scatter of Q_OS_ tests: the
//! UI, the capability gate and the admission rules have to agree, and they
//! only agree if they ask the same question.
//! NGPOST_FORCE_NO_VPN builds the unsupported-platform code paths on a
//! platform that does support the VPN. It exists so those paths -- the hidden
//! GUI controls, the inert admission rules -- can actually be compiled and
//! tested, instead of being verified only by reading them. Never define it in
//! a shipping build.
#if defined(NGPOST_FORCE_NO_VPN)
// deliberately leaves NGPOST_VPN_SUPPORTED undefined
#elif defined(Q_OS_LINUX) || defined(Q_OS_WIN)
#  define NGPOST_VPN_SUPPORTED 1
#endif

#endif // VPNPLATFORM_H
