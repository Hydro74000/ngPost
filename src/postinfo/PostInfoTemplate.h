// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Template engine for post info files.
//
// ngPost knows no third party format: the user provides a template file, this
// renders it. The very same variable table drives NZB_POST_CMD substitution,
// the environment passed to post commands, and the generated documentation.
//
//========================================================================

#ifndef POSTINFOTEMPLATE_H
#define POSTINFOTEMPLATE_H

#include "postinfo/PostInfoData.h"

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

class QProcessEnvironment;

//! Translation context of this module: "PostInfoTemplate".
namespace PostInfoTemplate
{

//! One substitutable variable. This table is the single source of truth: it
//! drives rendering, the NGPOST_* environment, and the documentation.
struct FieldDoc
{
    char const *placeholder; //!< "__nzbPath__"
    char const *envName;     //!< "NGPOST_NZB_PATH", declared, never derived
    char const *description; //!< plain English, translated at the point of use
    bool        isPath;      //!< native separators on Windows
    bool        isSecret;    //!< masked in logs, kept out of env/json by default
    //! True when the value already exists while the post is being prepared, so
    //! an editor can show it. The others only exist once the post is over:
    //! showing a 0 for them would read as a result, not as "not yet".
    bool knownBeforePost;
};

//! The fixed variables. Parameterized ones (__date__, __dateStart__,
//! __meta:key__) are handled by the renderer and documented separately.
QVector<FieldDoc> const &fields();

//! Name of the variable holding the archive password, for callers that need to
//! redact it without hardcoding the string.
QString const &passwordPlaceholder();

//! Resolved value of every fixed variable, keyed by its bare name (no __).
QMap<QString, QString> values(PostInfoData const &data, bool nativeSeparators);

//! NGPOST_* variables for a post command. Secrets are only added when
//! exposeSecrets is true; metadata never goes here, it goes in the json file.
void applyEnvironment(QProcessEnvironment &env, PostInfoData const &data, bool exposeSecrets);

//! Machine readable dump of the post, including metadata, for post commands.
QByteArray toJson(PostInfoData const &data, bool includeSecrets);

//! Substitution policy for unknown variables.
enum class OnUnknown
{
    KeepVerbatim, //!< body of a template, or a legacy NZB_POST_CMD line
    Fail          //!< output path: writing to an approximate location is worse
};

//! Renders one string. Unknown variables are reported through \a unknown.
//!
//! \a legacyPercentOne enables the historical "%1" of NZB_POST_CMD. It is off
//! everywhere else: in a record sheet, "50%1 off" is prose, not a request for
//! the nzb path.
QString render(QString const &tmpl,
               PostInfoData const &data,
               bool                nativeSeparators,
               OnUnknown           onUnknown        = OnUnknown::KeepVerbatim,
               QStringList        *unknown          = nullptr,
               bool                legacyPercentOne = false);

//! Renders each argument of an already split command line, so that a value
//! containing spaces or quotes stays exactly one argument.
QStringList renderArguments(QStringList const  &args,
                            PostInfoData const &data,
                            bool                nativeSeparators,
                            QStringList        *unknown = nullptr);

//! Names referenced by __meta:<name>__ in a template, in the order they
//! appear, without duplicates. Lets an editor offer exactly the fields the
//! chosen model asks for instead of leaving the user to guess them.
QStringList metaNamesIn(QString const &tmpl);

//! One variable as written in a template.
struct Token
{
    QString raw;  //!< "__date:dd/MM/yyyy__", exactly as the model spells it
    QString name; //!< "date"
    QString arg;  //!< "dd/MM/yyyy", empty when the variable takes none

    bool isMeta() const { return name == QLatin1String("meta"); }
};

//! Every variable a template uses, in the order it appears, without
//! duplicates. Unlike metaNamesIn() this also reports the ones ngPost fills in
//! by itself, so an editor can show the whole sheet and not just the blanks.
QVector<Token> tokensIn(QString const &tmpl);

//! Description of \a token, ready to show: the table entry for a fixed
//! variable, a sentence built on the spot for the parameterized ones. Empty
//! when the variable does not exist.
QString describe(Token const &token);

//! Replaces the secret values by "****". Used before logging a command.
QString redactSecrets(QString const &text, PostInfoData const &data);

//! Resolves a template path: absolute as-is, ~ expanded, relative against
//! \a baseDir (the config directory for a config value, the current directory
//! for a command line value). No ambiguous fallback.
QString resolveTemplatePath(QString const &path, QString const &baseDir);

struct Result
{
    bool        ok = false;
    QString     outPath;
    QString     error;
    QStringList warnings;
};

//! Reads \a templatePath, expands \a outputPattern to get the destination,
//! renders and writes it atomically. Never throws, never aborts a post: every
//! failure is reported in the returned Result.
//!
//! \a protectedPaths are refused as destination; a directory in that list also
//! protects everything below it. A relative destination is resolved against
//! \a outputBaseDir, which the configuration promises is its own folder.
Result renderToFile(QString const      &templatePath,
                    QString const      &outputPattern,
                    PostInfoData const &data,
                    QStringList const  &protectedPaths,
                    QString const      &outputBaseDir = QString());

} // namespace PostInfoTemplate

#endif // POSTINFOTEMPLATE_H
