//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef SECRETMASKER_H
#define SECRETMASKER_H

#include <QString>
#include <QStringList>

//! Keeps credentials out of anything ngPost writes for a human to read.
//!
//! The archivers take their password glued to the switch itself -- 7z wants
//! `-p<pass>`, rar `-hp<pass>` -- so the secret is an ordinary argv entry, and
//! a log line that prints the command prints the password with it. That line
//! runs in CLI mode, not only under --debug, so a job output pasted into a bug
//! report used to carry the archive password.
namespace SecretMasker
{
//! What replaces a secret. Fixed width: the length of the original must not
//! leak either.
QString mask();

//! \a arg with its secret replaced, or \a arg itself when it carries none.
//! Only the switch is kept, so a masked line still says which one was used.
QString maskedArg(QString const &arg);

//! maskedArg() over \a args, joined the way a command line reads. Use this
//! instead of QStringList::join() for anything that reaches a log.
QString maskedArgs(QStringList const &args);
}

#endif // SECRETMASKER_H
