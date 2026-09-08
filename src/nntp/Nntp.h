//========================================================================
//
// Copyright (C) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3..
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>
//
//========================================================================

#ifndef NNTP_H
#define NNTP_H

#include "utils/PureStaticClass.h"

#include <QByteArray>
#include <QString>

#include <map>
#include <regex>
#include <string>

/*!
 * \brief Pure Static class (no instance) to hold Nntp Protocol actions/responses...
 */
class Nntp : public PureStaticClass
{
public:
    static constexpr const char* QUIT          {"quit\r\n"};
    static constexpr const char* AUTHINFO_USER {"authinfo user "};
    static constexpr const char* AUTHINFO_PASS {"authinfo pass "};
    static constexpr const char* POST          {"post\r\n"};
    static constexpr const char* ENDLINE       {"\r\n"};
    static constexpr const char* STAT          {"stat"};

    //! RFC 3977 caps a message-id at 250 octets, angle brackets included.
    //! Anything longer is a malformed nzb, not an article somebody can post.
    static constexpr int MAX_MSG_ID_LEN {250};

    //! return the response associated to a certain code
    static const char* getResponse(unsigned short aCode);

    // ---- Command serialisation -----------------------------------------
    //
    // Every command ngPost puts on the wire is built here, and every one of
    // these returns either a single CRLF-terminated line or nothing at all.
    // Building them at the call site is what let a message-id carrying an
    // encoded CR/LF -- an nzb is an untrusted document -- append a second
    // command to an already authenticated session.

    //! True when \a messageId may be sent as a command argument: bracketed,
    //! printable US-ASCII, no space, no inner bracket, no control character,
    //! and within MAX_MSG_ID_LEN.
    static bool isValidMessageId(QString const &messageId);

    //! "stat <message-id>\r\n", or an empty array when \a messageId would not
    //! survive isValidMessageId().
    static QByteArray statCommand(QString const &messageId);

    //! "authinfo user <user>\r\n" / "authinfo pass <pass>\r\n", or an empty
    //! array when the credential carries a byte that would end the line early.
    //! Credentials come from the configuration file, which is edited by hand.
    static QByteArray authInfoUser(std::string const &user);
    static QByteArray authInfoPass(std::string const &pass);

    //! \a value with every byte that could end a header line, or forge a new
    //! one, replaced. For Subject, From, Newsgroups and the yEnc name, whose
    //! content comes from file names and from the configuration.
    static std::string sanitizedHeaderValue(std::string const &value);

private:
    static const std::map<unsigned short, const char *> sResponses; //!< Responses map
};

#endif // NNTP_H
