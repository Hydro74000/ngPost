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

#include "Nntp.h"

const std::map<unsigned short, const char *>  Nntp::sResponses = {
    {0,   "000 UNKNOWN NNTP RESPONSE..."},

    //rfc977: 2.4.3  General Responses
    {200, "200 server ready - posting allowed"},
    {201, "201 server ready - no posting allowed"},

    {400, "400 service discontinued"},
    {500, "500 command not recognized"},
    {501, "501 command syntax error"},
    {502, "502 access restriction or permission denied"},
    {503, "503 program fault - command couldn't perform"},


    //rfc977: 3.2.2  The GROUP command
    {211, "211 <number of articles> <first one> <last one> <name of the group>"},
    {411, "411 no such news group"},


    //rfc977: 3.10.2  The POST command
    {240, "240 article posted ok"},
    {340, "340 send article to be posted. End with <CR-LF>.<CR-LF>"},
    {440, "440 posting not allowed"},
    {441, "441 posting failed"},


    //rfc977: 6.2.4.  STAT
    {223, "223 0|n message-id    Article exists"},
    {430, "430 No article with that message-id"},


    //rfc977: 3.11.2  The QUIT command
    {205, "205 closing connection - goodbye!"},


    //rfc4643: 2.3.1
    {281, "281 Authentication accepted"},
    {380, "380 More Authentication Required"},
    {381, "381 Password required"},
    {480, "480 Authentication Required"},
    {481, "481 Authentication failed/rejected"},
    {482, "482 Authentication commands issued out of sequence"},
};

const char * Nntp::getResponse(unsigned short aCode){
    try {
        return sResponses.at(aCode);
    } catch (const std::out_of_range &ex) {
        return sResponses.at(0);
    }
}


namespace
{
//! True when \a value holds a byte that would terminate the line it is put on,
//! and so let whatever follows be read as a command or a header of its own.
bool carriesLineBreak(std::string const &value)
{
    for (char const c : value)
        if (c == '\r' || c == '\n' || c == '\0')
            return true;
    return false;
}

//! One CRLF-terminated line: \a prefix already ends with its separator.
QByteArray credentialCommand(char const *prefix, std::string const &value)
{
    if (carriesLineBreak(value))
        return QByteArray();

    QByteArray cmd(prefix);
    cmd += QByteArray::fromStdString(value);
    cmd += Nntp::ENDLINE;
    return cmd;
}
}

bool Nntp::isValidMessageId(QString const &messageId)
{
    int const len = messageId.size();
    if (len < 3 || len > MAX_MSG_ID_LEN)
        return false;
    if (!messageId.startsWith(QLatin1Char('<')) || !messageId.endsWith(QLatin1Char('>')))
        return false;

    for (int i = 1; i < len - 1; ++i) {
        char16_t const c = messageId.at(i).unicode();
        // Printable US-ASCII only. Below 0x21 covers NUL, CR, LF and the space
        // that would split the argument; 0x7F and above covers DEL and every
        // code point that toLocal8Bit() could turn into something else. An
        // inner bracket would close the id early and leave the rest as text
        // the server reads on its own terms.
        if (c <= 0x20 || c >= 0x7F || c == u'<' || c == u'>')
            return false;
    }
    return true;
}

QByteArray Nntp::statCommand(QString const &messageId)
{
    if (!isValidMessageId(messageId))
        return QByteArray();

    QByteArray cmd(STAT);
    cmd += ' ';
    cmd += messageId.toLatin1(); // validated above as printable US-ASCII
    cmd += ENDLINE;
    return cmd;
}

QByteArray Nntp::authInfoUser(std::string const &user)
{
    return credentialCommand(AUTHINFO_USER, user);
}

QByteArray Nntp::authInfoPass(std::string const &pass)
{
    return credentialCommand(AUTHINFO_PASS, pass);
}

std::string Nntp::sanitizedHeaderValue(std::string const &value)
{
    std::string clean;
    clean.reserve(value.size());
    for (char const c : value) {
        // Bytes at or above 0x80 are left alone: they are the UTF-8 the
        // headers already carry, and folding them would rename files.
        unsigned char const byte = static_cast<unsigned char>(c);
        clean += (byte < 0x20 || byte == 0x7F) ? '_' : c;
    }
    return clean;
}
