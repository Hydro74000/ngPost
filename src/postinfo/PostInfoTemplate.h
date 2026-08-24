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

//! How a substituted VALUE must be escaped before it lands in the sheet.
//!
//! It is deliberately a property of the model, not of a variable: a model is
//! either a text file, or a JSON document, or an XML one, and mixing the two
//! in a single file is not a thing a sheet does. A plain text model escapes
//! nothing at all -- that is what makes any separator, any wording and any
//! prose work.
enum class Escape
{
    None, //!< plain text: values are copied exactly as they are
    Json, //!< quotes, backslashes and control characters
    Xml   //!< & < > " '
};

//! Reads the format a model declares for itself. The declaration is a comment
//! line of its own, so it never reaches the produced file:
//!
//!     #!json
//!     #!xml
//!
//! Absent or unknown, the model is plain text and nothing is escaped.
//! \a unknownFormat, when given, receives the word of an unrecognised
//! directive so the caller can warn rather than escape by guesswork.
Escape escapeModeIn(QString const &tmpl, QString *unknownFormat = nullptr);

//! What escapeModeIn() reads, falling back on the name of the model when it
//! declares nothing: a model called sheet.json produces JSON.
//!
//! The directive always wins, including when it is unrecognised: an author who
//! wrote "#!yaml" said something, and silently escaping their file as JSON
//! because it happens to be named .json would contradict them. Only silence
//! lets the extension speak.
Escape escapeModeFor(QString const &tmpl,
                     QString const &templatePath,
                     QString       *unknownFormat = nullptr);

//! Escapes one value for \a mode. Escape::None returns it untouched.
QString escapeValue(QString const &value, Escape mode);

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
//! \a escape applies to the VALUES only, never to the text of the model: the
//! author writes the JSON braces or the XML tags and owns them.
QString render(QString const &tmpl,
               PostInfoData const &data,
               bool                nativeSeparators,
               OnUnknown           onUnknown        = OnUnknown::KeepVerbatim,
               QStringList        *unknown          = nullptr,
               bool                legacyPercentOne = false,
               Escape              escape           = Escape::None);

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

//! A line of a template, as the editor sees it. Every line of a file maps to
//! exactly one of these, and buildTemplate() is the exact inverse of
//! parseTemplate(): a model opened and saved again is unchanged.
struct SheetLine
{
    enum class Kind
    {
        Field, //!< "label =value", the only kind that produces a sheet entry
        Comment, //!< starts with '#' in the first column, never written out
        Raw //!< anything else: separators, blank lines, free prose
    };

    Kind    kind = Kind::Raw;
    QString label;      //!< Field: what is left of the first '='
    QString separator;  //!< Field: the exact "  =" in between, alignment kept
    QString expression; //!< Field: what is right of it, variables included
    QString raw;        //!< Comment and Raw: the line, untouched

    QString text() const
    {
        return kind == Kind::Field ? label + separator + expression : raw;
    }
};

//! Splits a template into its lines. Never fails: an unparsable line is Raw.
QVector<SheetLine> parseTemplate(QString const &tmpl);

//! Rebuilds the file from its lines, keeping the line ending style of the
//! original. buildTemplate(parseTemplate(t)) == t, for any t.
QString buildTemplate(QVector<SheetLine> const &lines, bool crlf = false);

//! True when \a tmpl uses CRLF, so a rewrite can keep the file as it was.
bool usesCrLf(QString const &tmpl);

//! Drops the comment lines. A line whose FIRST character is '#' is a comment
//! and is not written to the sheet; indent it by one space to have it written.
QString stripComments(QString const &tmpl);

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

//! A rendered model held in memory. This is the common pipeline used by file
//! output and by CLI stdout: it reads the model, removes comments/directives,
//! applies the declared (or filename-derived) escaping, and reports unknown
//! variables without ever creating an output file.
struct RenderResult
{
    bool        ok = false;
    QString     text;
    QString     error;
    QStringList warnings;
    Escape      escape = Escape::None;
};

RenderResult renderTemplateFile(QString const &templatePath, PostInfoData const &data);

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
