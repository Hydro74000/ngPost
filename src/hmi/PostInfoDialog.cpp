// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>

#include "PostInfoDialog.h"

#include "CheckBoxCenterWidget.h"
#include "postinfo/PostInfoTemplate.h"

#include <QCheckBox>
#include <QSignalBlocker>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace
{
//! Deterministic names, so the GUI tests can reach the cells.
QString fieldName(char const *role, int row)
{
    return QStringLiteral("%1_%2").arg(QLatin1String(role)).arg(row);
}
} // namespace

PostInfoDialog::PostInfoDialog(const QString &configuredTemplate,
                               const QString &templateOverride,
                               const QMap<QString, MetaValue> &meta,
                               const QStringList &sessionTemplates,
                               const PostInfoData &preview,
                               QWidget *parent)
    : QDialog(parent), _configuredTemplate(configuredTemplate), _preview(preview),
      _sessionTemplates(sessionTemplates)
{
    setObjectName(QStringLiteral("postInfoDialog"));
    setWindowTitle(tr("Post information"));
    setModal(true);

    QVBoxLayout *root = new QVBoxLayout(this);

    root->addWidget(new QLabel(tr("A post info file describes this post in a text file written "
                                  "next to the nzb.\nYou give the model, ngPost fills in the "
                                  "blanks."),
                               this));

    // ---- the model -------------------------------------------------------
    QHBoxLayout *tmplRow = new QHBoxLayout();
    tmplRow->addWidget(new QLabel(tr("Model:"), this));

    _templateList = new QComboBox(this);
    _templateList->setObjectName(QStringLiteral("postInfoTemplateList"));
    tmplRow->addWidget(_templateList, 1);

    // A small cross to drop a model from the list. Next to it rather than
    // inside each row: a combo box has no per item button, and a half working
    // one in a popup is worse than an obvious button.
    _forgetButton = new QPushButton(QString(QChar(0x2715)), this);
    _forgetButton->setObjectName(QStringLiteral("postInfoForgetButton"));
    _forgetButton->setFixedWidth(34);
    _forgetButton->setToolTip(tr("Remove this model from the list"));
    tmplRow->addWidget(_forgetButton);

    _loadFieldsButton = new QPushButton(tr("Read its fields"), this);
    _loadFieldsButton->setObjectName(QStringLiteral("postInfoLoadFieldsButton"));
    _loadFieldsButton->setToolTip(tr("Offer exactly the fields this model asks for."));
    tmplRow->addWidget(_loadFieldsButton);

    root->addLayout(tmplRow);

    _setAsDefault = new QCheckBox(tr("Use this model for my next posts too"), this);
    _setAsDefault->setObjectName(QStringLiteral("postInfoSetAsDefault"));
    _setAsDefault->setToolTip(
        tr("Writes it in your configuration as POST_INFO_TEMPLATE, so it becomes\n"
           "the model offered by default. This post uses it either way."));
    root->addWidget(_setAsDefault);

    _templateHint = new QLabel(this);
    _templateHint->setObjectName(QStringLiteral("postInfoTemplateHint"));
    _templateHint->setWordWrap(true);
    root->addWidget(_templateHint);

    // ---- the fields ------------------------------------------------------
    root->addWidget(new QLabel(tr("Below is the sheet this model produces. The lines named "
                                  "__like_this__ are filled in by ngPost;\nthe others are yours. "
                                  "A field of yours always goes into the file \342\200\224 tick "
                                  "\302\253 Also in NZB \302\273 to publish it in the nzb too."),
                               this));

    _fields = new QTableWidget(this);
    _fields->setObjectName(QStringLiteral("postInfoFieldsTable"));
    _fields->setColumnCount(4);
    _fields->verticalHeader()->hide();
    _fields->setHorizontalHeaderLabels(
        QStringList{ tr("Name"), tr("Value"), tr("Also in NZB"), QString() });
    _fields->horizontalHeaderItem(2)->setToolTip(
        tr("Off: the field is written in the post info file only.\n"
           "On: it is written there AND published in the nzb, which circulates."));
    _fields->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    // Wide enough for the longest variable name: a truncated "__nbArticl..."
    // tells the user nothing.
    _fields->setColumnWidth(0, 200);
    _fields->setColumnWidth(2, 90);
    _fields->setColumnWidth(3, 30);
    root->addWidget(_fields, 1);

    QHBoxLayout *actionRow = new QHBoxLayout();
    _addButton = new QPushButton(tr("Add a field"), this);
    _addButton->setObjectName(QStringLiteral("postInfoAddFieldButton"));
    actionRow->addWidget(_addButton);
    actionRow->addStretch();

    _helpButton = new QPushButton(tr("?  What can I put in a model"), this);
    _helpButton->setObjectName(QStringLiteral("postInfoHelpButton"));
    _helpButton->setToolTip(tr("Every __variable__ ngPost knows, and what it holds."));
    actionRow->addWidget(_helpButton);
    root->addLayout(actionRow);


    QDialogButtonBox *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(_addButton, &QAbstractButton::clicked, this, &PostInfoDialog::onAddField);
    connect(_forgetButton, &QAbstractButton::clicked, this, &PostInfoDialog::onForgetTemplate);
    connect(_helpButton, &QAbstractButton::clicked, this, &PostInfoDialog::onShowHelp);
    connect(_loadFieldsButton,
            &QAbstractButton::clicked,
            this,
            &PostInfoDialog::onLoadFieldsFromTemplate);

    _fillTemplateList(templateOverride);
    connect(_templateList,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &PostInfoDialog::onTemplateChosen);

    for (auto it = meta.cbegin(); it != meta.cend(); ++it)
        _addField(it.key(), it.value().value, it.value().scope == MetaScope::Nzb);

    // Opening on an empty editor with a model already known is the common case:
    // show what it asks for straight away.
    if (meta.isEmpty() && !_effectiveTemplatePath().isEmpty())
        onLoadFieldsFromTemplate();
    else
        _templateHint->setText(_effectiveTemplatePath().isEmpty()
                                   ? tr("No model: nothing will be written for this post.")
                                   : tr("Model in use: %1").arg(_effectiveTemplatePath()));

    resize(620, 420);
}

QString PostInfoDialog::templateOverride() const
{
    // The default is not an override: leaving it selected means "whatever the
    // configuration says", so a later change there follows this post too.
    QString const chosen = _effectiveTemplatePath();
    return chosen == _configuredTemplate ? QString() : chosen;
}

bool PostInfoDialog::setAsDefault() const
{
    return _setAsDefault->isChecked() && !_effectiveTemplatePath().isEmpty();
}

QString PostInfoDialog::_effectiveTemplatePath() const
{
    return _templateList->currentData().toString();
}

//! The configured model first, marked as such, then the ones opened earlier in
//! this run, then a way out to the filesystem.
void PostInfoDialog::_fillTemplateList(const QString &selected)
{
    // Rebuilding a combo box moves its current index around, and every move
    // emits currentIndexChanged. Without this guard, inserting an item while
    // "Choose a file..." was selected reopened the file dialog.
    QSignalBlocker const blocker(_templateList);
    _fillingList = true;

    _templateList->clear();
    if (!_configuredTemplate.isEmpty())
        _templateList->addItem(tr("%1  (default)").arg(QFileInfo(_configuredTemplate).fileName()),
                               _configuredTemplate);
    else
        _templateList->addItem(tr("(no model yet)"), QString());

    for (QString const &path : _sessionTemplates) {
        if (path != _configuredTemplate)
            _templateList->addItem(QFileInfo(path).fileName(), path);
    }
    if (!selected.isEmpty() && selected != _configuredTemplate
        && !_sessionTemplates.contains(selected))
        _templateList->addItem(QFileInfo(selected).fileName(), selected);

    _templateList->addItem(tr("Choose a file\342\200\246"), QStringLiteral("__browse__"));

    int index = 0;
    if (!selected.isEmpty()) {
        int const found = _templateList->findData(selected);
        if (found >= 0)
            index = found;
    }
    _templateList->setCurrentIndex(index);
    _fillingList = false;
    _updateForgetButton();
}

void PostInfoDialog::_rememberTemplate(const QString &path)
{
    if (!path.isEmpty() && path != _configuredTemplate && !_sessionTemplates.contains(path))
        _sessionTemplates << path;
    _fillTemplateList(path);
}

//! The configured model is not ours to drop, and neither is the way out to the
//! filesystem.
void PostInfoDialog::_updateForgetButton()
{
    QString const current = _effectiveTemplatePath();
    _forgetButton->setEnabled(!current.isEmpty() && current != QLatin1String("__browse__")
                              && _sessionTemplates.contains(current));
}

void PostInfoDialog::onForgetTemplate()
{
    QString const current = _effectiveTemplatePath();
    if (!_sessionTemplates.removeAll(current))
        return;
    _fillTemplateList(QString()); // falls back on the default
    onLoadFieldsFromTemplate();
}

void PostInfoDialog::onTemplateChosen(int index)
{
    if (_fillingList)
        return;

    if (_templateList->itemData(index).toString() != QLatin1String("__browse__")) {
        _updateForgetButton();
        onLoadFieldsFromTemplate();
        return;
    }

    QString const path = QFileDialog::getOpenFileName(
        this, tr("Post info model"), QString(), tr("Text files (*.txt *.tpl);;All files (*)"));
    if (path.isEmpty()) {
        _fillTemplateList(QString()); // back to the default
        return;
    }
    _rememberTemplate(path);
    onLoadFieldsFromTemplate();
}

void PostInfoDialog::onLoadFieldsFromTemplate()
{
    QString const path = _effectiveTemplatePath();
    if (path.isEmpty()) {
        _templateHint->setText(tr("No model: nothing will be written for this post."));
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        _templateHint->setText(tr("Cannot read %1").arg(path));
        return;
    }
    QVector<PostInfoTemplate::Token> const tokens =
        PostInfoTemplate::tokensIn(QString::fromUtf8(file.readAll()));
    file.close();

    // What the user already typed survives a change of model: it is keyed by
    // name, so a field both models ask for keeps its value and its NZB box.
    QString                  duplicate;
    QMap<QString, MetaValue> typed = meta(&duplicate);

    // Rebuilt rather than completed, so the table follows the order of the
    // model: read top to bottom it is the sheet that will be produced.
    _fields->setRowCount(0);

    QMap<QString, QString> const values = PostInfoTemplate::values(_preview, false);

    int asked = 0;
    for (PostInfoTemplate::Token const &token : tokens) {
        if (token.isMeta()) {
            if (token.arg.isEmpty())
                continue; // "__meta__" with no name asks for nothing
            _addField(token.arg, typed.value(token.arg).value,
                      typed.value(token.arg).scope == MetaScope::Nzb);
            typed.remove(token.arg);
            ++asked;
            continue;
        }

        QString const description = PostInfoTemplate::describe(token);
        if (description.isEmpty()) {
            _addAutoField(token.raw, QString(), tr("unknown variable, copied as it is"));
            continue;
        }
        _addAutoField(token.raw, _previewValue(token, values), description);
    }

    // Fields the previous model asked for and this one does not: kept at the
    // end rather than dropped, because throwing away typed text is worse than
    // showing a line the model ignores.
    for (auto it = typed.cbegin(); it != typed.cend(); ++it)
        _addField(it.key(), it.value().value, it.value().scope == MetaScope::Nzb);

    if (asked == 0)
        _templateHint->setText(tr("Model in use: %1 (it asks for no field of yours)").arg(path));
    else
        _templateHint->setText(tr("Model in use: %1 \342\200\224 %n field(s) to fill in", "", asked)
                                   .arg(path));
}

//! The values that already exist while the post is being prepared. The others
//! only exist once it is over, and are announced as such: showing "0 articles"
//! before posting would read as a result rather than as "not yet".
QString PostInfoDialog::_previewValue(PostInfoTemplate::Token const  &token,
                                      QMap<QString, QString> const &values) const
{
    if (token.name == QLatin1String("date") || token.name == QLatin1String("dateStart"))
        return QDateTime::currentDateTime().toString(
            token.arg.isEmpty() ? QStringLiteral("yyyy-MM-dd") : token.arg);

    QString const placeholder = QStringLiteral("__%1__").arg(token.name);
    for (PostInfoTemplate::FieldDoc const &field : PostInfoTemplate::fields()) {
        if (placeholder != QLatin1String(field.placeholder))
            continue;
        if (!field.knownBeforePost)
            return QString();
        // A secret is not printed on screen just to preview a layout: knowing
        // it is there is what the user needs.
        if (field.isSecret)
            return values.value(token.name).isEmpty() ? QString()
                                                      : QStringLiteral("\342\200\242\342\200\242\342\200\242\342\200\242\342\200\242\342\200\242");
        return values.value(token.name);
    }
    return QString();
}

//! The reference of every variable, built from the engine table so it cannot
//! drift from what the renderer actually supports.
void PostInfoDialog::onShowHelp()
{
    QString html = QStringLiteral("<h3>%1</h3><p>%2</p>")
                       .arg(tr("What can I put in a model"),
                            tr("A model is a plain text file. Write it as you want the sheet to "
                               "read, and put a variable wherever ngPost should fill something "
                               "in. Anything ngPost does not recognise is copied as it is."));

    html += QStringLiteral("<pre>%1</pre>")
                .arg(QStringLiteral("name  =__originalName__\n"
                                    "size  =__postSize__\n"
                                    "title =__meta:title__"));

    html += QStringLiteral("<h4>%1</h4><p>%2</p>")
                .arg(tr("Your own fields"),
                     tr("<b>__meta:name__</b> is a field of yours. Every name you use here "
                        "appears in the table of this window, waiting for a value. It always "
                        "goes into the post info file; tick <i>Also in NZB</i> to publish it "
                        "in the nzb as well."));

    html += QStringLiteral("<h4>%1</h4><p>%2</p>")
                .arg(tr("Dates"),
                     tr("<b>__date__</b> and <b>__dateStart__</b> are the end and the start of "
                        "the post. Give them a format after a colon, for instance "
                        "<b>__date:dd/MM/yyyy__</b> or <b>__date:yyyy-MM-dd HH:mm__</b>. "
                        "Without a format you get <b>yyyy-MM-dd</b>. The time is yours, not UTC."));

    html += QStringLiteral("<h4>%1</h4>").arg(tr("Filled in by ngPost"));
    html += QStringLiteral("<table cellspacing='0' cellpadding='4' border='0'>");
    html += QStringLiteral("<tr><th align='left'>%1</th><th align='left'>%2</th>"
                           "<th align='left'>%3</th></tr>")
                .arg(tr("Variable"), tr("What it holds"), tr("Known"));

    bool anySecret = false;
    for (PostInfoTemplate::FieldDoc const &field : PostInfoTemplate::fields()) {
        QString name = QString::fromLatin1(field.placeholder).toHtmlEscaped();
        if (field.isSecret) {
            name += QStringLiteral(" \342\232\240");
            anySecret = true;
        }
        html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td><td>%3</td></tr>")
                    .arg(name,
                         QCoreApplication::translate("PostInfoTemplate", field.description)
                             .toHtmlEscaped(),
                         field.knownBeforePost ? tr("before the post") : tr("after the post"));
    }
    html += QStringLiteral("</table>");

    html += QStringLiteral("<p>%1</p>")
                .arg(tr("<i>before the post</i> means the value is already known while you "
                        "prepare it, so this window shows it. <i>after the post</i> means it "
                        "only exists once the post is over, which is why it is blank here."));

    if (anySecret)
        html += QStringLiteral("<p>\342\232\240 %1</p>")
                    .arg(tr("A variable marked with a warning sign holds a secret. Putting it in "
                            "a model makes the produced file as sensitive as the password itself: "
                            "write it in a folder only you can read."));

    QDialog help(this);
    help.setObjectName(QStringLiteral("postInfoHelpDialog"));
    help.setWindowTitle(tr("Post info variables"));
    QVBoxLayout *layout = new QVBoxLayout(&help);

    QTextBrowser *text = new QTextBrowser(&help);
    text->setObjectName(QStringLiteral("postInfoHelpText"));
    text->setOpenExternalLinks(false);
    text->setHtml(html);
    layout->addWidget(text);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &help);
    connect(buttons, &QDialogButtonBox::rejected, &help, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &help, &QDialog::accept);
    layout->addWidget(buttons);

    help.resize(720, 560);
    help.exec();
}

void PostInfoDialog::onAddField() { _addField(QString(), QString(), false); }

void PostInfoDialog::_addField(const QString &name, const QString &value, bool publish)
{
    int const row = _fields->rowCount();
    _fields->insertRow(row);

    QLineEdit *nameEdit = new QLineEdit(name, _fields);
    nameEdit->setObjectName(fieldName("postInfoFieldName", row));
    nameEdit->setPlaceholderText(tr("album"));
    _fields->setCellWidget(row, 0, nameEdit);

    QLineEdit *valueEdit = new QLineEdit(value, _fields);
    valueEdit->setObjectName(fieldName("postInfoFieldValue", row));
    _fields->setCellWidget(row, 1, valueEdit);

    auto *publishCell = new CheckBoxCenterWidget(_fields, publish);
    publishCell->setObjectName(fieldName("postInfoFieldNzb", row));
    _fields->setCellWidget(row, 2, publishCell);

    QPushButton *del = new QPushButton(QString(QChar(0x2715)), _fields);
    del->setObjectName(fieldName("postInfoFieldDel", row));
    del->setMaximumWidth(30);
    connect(del, &QAbstractButton::clicked, this, [this, del]() {
        for (int i = 0; i < _fields->rowCount(); ++i) {
            if (_fields->cellWidget(i, 3) == del) {
                _fields->removeRow(i);
                break;
            }
        }
    });
    _fields->setCellWidget(row, 3, del);
}

//! A line the model asks for and ngPost fills in on its own. It is shown so
//! the table is the sheet, not just the blanks in it; it cannot be edited or
//! removed, because only the model decides whether it is there.
void PostInfoDialog::_addAutoField(const QString &placeholder,
                                   const QString &value,
                                   const QString &description)
{
    int const row = _fields->rowCount();
    _fields->insertRow(row);

    QLineEdit *nameEdit = new QLineEdit(placeholder, _fields);
    nameEdit->setObjectName(fieldName("postInfoAutoName", row));
    nameEdit->setReadOnly(true);
    nameEdit->setFrame(false);
    nameEdit->setToolTip(description);
    nameEdit->setCursorPosition(0); // show the start, not the tail
    _fields->setCellWidget(row, 0, nameEdit);

    QLineEdit *valueEdit = new QLineEdit(_fields);
    valueEdit->setObjectName(fieldName("postInfoAutoValue", row));
    valueEdit->setReadOnly(true);
    valueEdit->setFrame(false);
    valueEdit->setToolTip(description);
    if (value.isEmpty()) {
        // The description alone, not "filled in after the post — <description>"
        // on every line: repeated ten times it stops being read at all. The
        // point is carried once by the sentence above the table.
        valueEdit->setPlaceholderText(description);
    } else {
        valueEdit->setText(value);
        valueEdit->setCursorPosition(0);
    }
    _fields->setCellWidget(row, 1, valueEdit);

    // Columns 2 and 3 stay empty: an automatic line is not published on
    // request, and cannot be removed from here.
    _fields->setItem(row, 2, new QTableWidgetItem(QString()));
    _fields->item(row, 2)->setFlags(Qt::NoItemFlags);
    _fields->setItem(row, 3, new QTableWidgetItem(QString()));
    _fields->item(row, 3)->setFlags(Qt::NoItemFlags);
}

QMap<QString, MetaValue> PostInfoDialog::meta(QString *duplicate) const
{
    QMap<QString, MetaValue> meta;
    for (int row = 0; row < _fields->rowCount(); ++row) {
        auto *nameEdit  = qobject_cast<QLineEdit *>(_fields->cellWidget(row, 0));
        auto *valueEdit = qobject_cast<QLineEdit *>(_fields->cellWidget(row, 1));
        if (!nameEdit || !valueEdit)
            continue;
        if (nameEdit->isReadOnly())
            continue; // a line ngPost fills in by itself, not one of yours

        QString const name = nameEdit->text().trimmed();
        if (name.isEmpty())
            continue; // an empty line is just an unused one
        // "password" is a secret and travels through the password field
        if (name.compare(QStringLiteral("password"), Qt::CaseInsensitive) == 0)
            continue;
        if (duplicate && meta.contains(name)) {
            *duplicate = name;
            return meta;
        }

        bool publish = false;
        if (auto *cell = qobject_cast<CheckBoxCenterWidget *>(_fields->cellWidget(row, 2)))
            publish = cell->isChecked();

        meta.insert(name, MetaValue(valueEdit->text(), publish ? MetaScope::Nzb : MetaScope::Local));
    }
    return meta;
}
