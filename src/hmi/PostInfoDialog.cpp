// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>

#include "PostInfoDialog.h"

#include "CheckBoxCenterWidget.h"
#include "postinfo/PostInfoTemplate.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace
{
//! Deterministic names, so the GUI tests can reach the cells.
QString cellName(char const *role, int row)
{
    return QStringLiteral("%1_%2").arg(QLatin1String(role)).arg(row);
}

//! Columns of the model table.
enum ModelCol
{
    ColLabel = 0,
    ColExpression,
    ColPreview,
    ColDelLine
};

//! Columns of the values table.
enum FieldCol
{
    ColName = 0,
    ColValue,
    ColNzb,
    ColDelField
};
} // namespace

PostInfoDialog::PostInfoDialog(const QString                  &configuredTemplate,
                               const QString                  &templateOverride,
                               const QMap<QString, MetaValue> &meta,
                               const QStringList              &sessionTemplates,
                               const PostInfoData             &preview,
                               QWidget                        *parent)
    : QDialog(parent)
    , _configuredTemplate(configuredTemplate)
    , _preview(preview)
    , _sessionTemplates(sessionTemplates)
{
    setObjectName(QStringLiteral("postInfoDialog"));
    setWindowTitle(tr("Post information"));
    setModal(true);

    QVBoxLayout *root = new QVBoxLayout(this);

    root->addWidget(new QLabel(tr("A post info file describes this post in a text file written "
                                  "next to the nzb.\nYou give the model, ngPost fills in the "
                                  "blanks."),
                               this));

    // ---- which model -----------------------------------------------------
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

    _reloadButton = new QPushButton(tr("Reload"), this);
    _reloadButton->setObjectName(QStringLiteral("postInfoLoadFieldsButton"));
    _reloadButton->setToolTip(tr("Read the file again, dropping the changes made here."));
    tmplRow->addWidget(_reloadButton);

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

    // ---- the model: the file, line by line -------------------------------
    QGroupBox   *modelBox    = new QGroupBox(tr("The model \342\200\224 the file, line by line"), this);
    QVBoxLayout *modelLayout = new QVBoxLayout(modelBox);
    modelLayout->addWidget(new QLabel(
        tr("This is the file itself. Anything in a line is written as it is, and every "
           "__variable__ is replaced by its value.\nA line starting with # is a comment: it is "
           "never written. Changes here only reach the file through \302\253 Save as\342\200\246 \302\273."),
        modelBox));

    _model = new QTableWidget(modelBox);
    _model->setObjectName(QStringLiteral("postInfoModelTable"));
    _model->setColumnCount(4);
    _model->verticalHeader()->hide();
    _model->setHorizontalHeaderLabels(
        QStringList{ tr("Field"), tr("Content of the line"), tr("What will come out"), QString() });
    _model->horizontalHeader()->setSectionResizeMode(ColExpression, QHeaderView::Stretch);
    _model->horizontalHeader()->setSectionResizeMode(ColPreview, QHeaderView::Stretch);
    _model->setColumnWidth(ColLabel, 150);
    _model->setColumnWidth(ColDelLine, 34);
    modelLayout->addWidget(_model, 1);

    QHBoxLayout *modelActions = new QHBoxLayout();
    _addLineButton            = new QPushButton(tr("Add a line"), modelBox);
    _addLineButton->setObjectName(QStringLiteral("postInfoAddLineButton"));
    _addLineButton->setToolTip(tr("Inserts a line under the selected one, at the end otherwise."));
    modelActions->addWidget(_addLineButton);

    _saveAsButton = new QPushButton(tr("Save as\342\200\246"), modelBox);
    _saveAsButton->setObjectName(QStringLiteral("postInfoSaveAsButton"));
    _saveAsButton->setToolTip(tr("Writes these lines to a model file of your own."));
    modelActions->addWidget(_saveAsButton);
    modelActions->addStretch();
    modelLayout->addLayout(modelActions);

    root->addWidget(modelBox, 3);

    // ---- your fields: the values of THIS post ----------------------------
    QGroupBox   *fieldsBox    = new QGroupBox(tr("Your fields \342\200\224 the values of this post"), this);
    QVBoxLayout *fieldsLayout = new QVBoxLayout(fieldsBox);
    fieldsLayout->addWidget(new QLabel(
        tr("One line per __meta:name__ the model uses. These values belong to this post, not to "
           "the model:\nthey are never written into the file above. A field always goes into the "
           "post info file \342\200\224 tick \302\253 Also in NZB \302\273 to publish it in the nzb too."),
        fieldsBox));

    _fields = new QTableWidget(fieldsBox);
    _fields->setObjectName(QStringLiteral("postInfoFieldsTable"));
    _fields->setColumnCount(4);
    _fields->verticalHeader()->hide();
    _fields->setHorizontalHeaderLabels(
        QStringList{ tr("Name"), tr("Value"), tr("Also in NZB"), QString() });
    _fields->horizontalHeaderItem(ColNzb)->setToolTip(
        tr("Off: the field is written in the post info file only.\n"
           "On: it is written there AND published in the nzb, which circulates."));
    _fields->horizontalHeader()->setSectionResizeMode(ColValue, QHeaderView::Stretch);
    _fields->setColumnWidth(ColName, 150);
    _fields->setColumnWidth(ColNzb, 90);
    _fields->setColumnWidth(ColDelField, 34);
    fieldsLayout->addWidget(_fields, 1);

    QHBoxLayout *fieldActions = new QHBoxLayout();
    _addFieldButton           = new QPushButton(tr("Add a field"), fieldsBox);
    _addFieldButton->setObjectName(QStringLiteral("postInfoAddFieldButton"));
    _addFieldButton->setToolTip(tr("Adds a field of yours, and the line that writes it."));
    fieldActions->addWidget(_addFieldButton);
    fieldActions->addStretch();
    fieldsLayout->addLayout(fieldActions);

    root->addWidget(fieldsBox, 2);

    // ---- the way out -----------------------------------------------------
    QHBoxLayout *bottom = new QHBoxLayout();
    _helpButton         = new QPushButton(tr("?  What can I put in a model"), this);
    _helpButton->setObjectName(QStringLiteral("postInfoHelpButton"));
    _helpButton->setToolTip(tr("Every __variable__ ngPost knows, and what it holds."));
    bottom->addWidget(_helpButton);
    bottom->addStretch();

    QDialogButtonBox *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bottom->addWidget(buttons);
    root->addLayout(bottom);

    connect(buttons, &QDialogButtonBox::accepted, this, &PostInfoDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(_forgetButton, &QAbstractButton::clicked, this, &PostInfoDialog::onForgetTemplate);
    connect(_helpButton, &QAbstractButton::clicked, this, &PostInfoDialog::onShowHelp);
    connect(_reloadButton, &QAbstractButton::clicked, this, &PostInfoDialog::onReloadModel);
    connect(_addLineButton, &QAbstractButton::clicked, this, &PostInfoDialog::onAddModelLine);
    connect(_saveAsButton, &QAbstractButton::clicked, this, &PostInfoDialog::onSaveModelAs);
    connect(_addFieldButton, &QAbstractButton::clicked, this, &PostInfoDialog::onAddField);

    _fillTemplateList(templateOverride);
    connect(_templateList,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &PostInfoDialog::onTemplateChosen);

    // The values of the post come first: loading the model then completes them
    // with whatever it asks for and this post has not answered yet.
    for (auto it = meta.cbegin(); it != meta.cend(); ++it)
        _addField(it.key(), it.value().value, it.value().scope == MetaScope::Nzb);

    _loadModel();

    resize(900, 760);
}

// ======================================================================
//  which model
// ======================================================================

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
    QString const data = _templateList->currentData().toString();
    return data == QLatin1String("__browse__") ? QString() : data;
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
    _forgetButton->setEnabled(!current.isEmpty() && _sessionTemplates.contains(current));
}

void PostInfoDialog::onForgetTemplate()
{
    QString const current = _effectiveTemplatePath();
    if (!_sessionTemplates.removeAll(current))
        return;
    _fillTemplateList(QString()); // falls back on the default
    _loadModel();
}

void PostInfoDialog::onTemplateChosen(int index)
{
    if (_fillingList)
        return;

    if (_templateList->itemData(index).toString() != QLatin1String("__browse__")) {
        _updateForgetButton();
        _loadModel();
        return;
    }

    QString const path = QFileDialog::getOpenFileName(
        this, tr("Post info model"), QString(), tr("Text files (*.txt *.tpl);;All files (*)"));
    if (path.isEmpty()) {
        _fillTemplateList(QString()); // back to the default
        return;
    }
    _rememberTemplate(path);
    _loadModel();
}

void PostInfoDialog::onReloadModel() { _loadModel(); }

// ======================================================================
//  the model: the file, line by line
// ======================================================================

void PostInfoDialog::_loadModel()
{
    QString const path = _effectiveTemplatePath();
    _lines.clear();
    _crlf   = false;
    _escape = PostInfoTemplate::Escape::None;

    if (path.isEmpty()) {
        _templateHint->setText(tr("No model: nothing will be written for this post."));
    } else {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            _templateHint->setText(tr("Cannot read %1").arg(path));
        } else {
            QString const text = QString::fromUtf8(file.readAll());
            file.close();
            _crlf   = PostInfoTemplate::usesCrLf(text);
            _lines  = PostInfoTemplate::parseTemplate(text);
            _escape = PostInfoTemplate::escapeModeFor(text, path);
            _templateHint->setText(tr("Model in use: %1").arg(path));
        }
    }

    _setModelDirty(false);
    _fillModelTable();
    _syncFieldsFromModel();
    _refreshPreviews();
}

void PostInfoDialog::_fillModelTable()
{
    _building = true;
    _model->clearSpans();
    _model->setRowCount(0);

    for (int i = 0; i < _lines.size(); ++i) {
        PostInfoTemplate::SheetLine const &line = _lines.at(i);
        int const                          row  = _model->rowCount();
        _model->insertRow(row);

        if (line.kind == PostInfoTemplate::SheetLine::Kind::Field) {
            QLineEdit *label = new QLineEdit(line.label, _model);
            label->setObjectName(cellName("postInfoModelLabel", row));
            label->setPlaceholderText(tr("name of the line"));
            connect(label, &QLineEdit::textEdited, this, [this, i](QString const &text) {
                if (i < _lines.size()) {
                    _lines[i].label = text;
                    _setModelDirty(true);
                }
            });
            _model->setCellWidget(row, ColLabel, label);

            QLineEdit *expr = new QLineEdit(line.expression, _model);
            expr->setObjectName(cellName("postInfoModelExpr", row));
            expr->setPlaceholderText(tr("text, __variables__, or both"));
            connect(expr, &QLineEdit::textEdited, this, [this, i](QString const &text) {
                if (i < _lines.size()) {
                    _lines[i].expression = text;
                    _setModelDirty(true);
                    _syncFieldsFromModel();
                    _refreshPreviews();
                }
            });
            _model->setCellWidget(row, ColExpression, expr);

            QLineEdit *preview = new QLineEdit(_model);
            preview->setObjectName(cellName("postInfoModelPreview", row));
            preview->setReadOnly(true);
            preview->setFrame(false);
            _model->setCellWidget(row, ColPreview, preview);
        } else {
            // A comment or a free line has no label and no value: it is one
            // piece of text, so it gets one cell across the two columns.
            QLineEdit *raw = new QLineEdit(line.raw, _model);
            raw->setObjectName(cellName("postInfoModelRaw", row));
            bool const isComment = line.kind == PostInfoTemplate::SheetLine::Kind::Comment;
            raw->setPlaceholderText(isComment ? tr("comment, never written")
                                              : tr("free text, written as it is"));
            raw->setToolTip(isComment
                                ? tr("A line starting with # is a comment. Indent it by one "
                                     "space to have it written.")
                                : tr("Written to the sheet exactly as it reads here."));
            connect(raw, &QLineEdit::textEdited, this, [this, i](QString const &text) {
                if (i >= _lines.size())
                    return;
                _lines[i].raw = text;
                // Typing a '#' in the first column turns the line into a
                // comment straight away, and removing it brings it back.
                _lines[i].kind = text.startsWith(QLatin1Char('#'))
                                     ? PostInfoTemplate::SheetLine::Kind::Comment
                                     : PostInfoTemplate::SheetLine::Kind::Raw;
                _setModelDirty(true);
                _refreshPreviews();
            });
            _model->setCellWidget(row, ColLabel, raw);
            _model->setSpan(row, ColLabel, 1, 2);

            QLineEdit *preview = new QLineEdit(_model);
            preview->setObjectName(cellName("postInfoModelPreview", row));
            preview->setReadOnly(true);
            preview->setFrame(false);
            _model->setCellWidget(row, ColPreview, preview);
        }

        QPushButton *del = new QPushButton(QString(QChar(0x2715)), _model);
        del->setObjectName(cellName("postInfoModelDel", row));
        del->setFixedWidth(30);
        del->setToolTip(tr("Remove this line from the model"));
        connect(del, &QAbstractButton::clicked, this, [this, i]() { _removeModelLine(i); });
        _model->setCellWidget(row, ColDelLine, del);
    }

    _building = false;
}

void PostInfoDialog::_removeModelLine(int lineIndex)
{
    if (lineIndex < 0 || lineIndex >= _lines.size())
        return;
    _lines.remove(lineIndex);
    _setModelDirty(true);
    _fillModelTable();
    _syncFieldsFromModel();
    _refreshPreviews();
}

void PostInfoDialog::onAddModelLine()
{
    PostInfoTemplate::SheetLine line;
    line.kind      = PostInfoTemplate::SheetLine::Kind::Field;
    line.separator = QStringLiteral(" =");

    // Under the selected line, so a model can be composed in the order it will
    // be read; at the end when nothing is selected.
    int const current = _model->currentRow();
    int const at      = (current >= 0 && current < _lines.size()) ? current + 1 : _lines.size();
    _lines.insert(at, line);

    _setModelDirty(true);
    _fillModelTable();
    _refreshPreviews();

    _model->setCurrentCell(at, ColLabel);
    if (auto *edit = qobject_cast<QLineEdit *>(_model->cellWidget(at, ColLabel)))
        edit->setFocus();
}

//! Renders every line with the values of this post, so the third column is
//! literally what the sheet will hold.
void PostInfoDialog::_refreshPreviews()
{
    PostInfoData data = _preview;
    data.meta         = meta(); // what is typed right now, duplicates ignored here

    // A date is knowable while the post is being prepared: previewing today
    // shows the shape of the line, which is what the format is chosen for.
    if (!data.finishedAt.isValid())
        data.finishedAt = QDateTime::currentDateTime();
    if (!data.startedAt.isValid())
        data.startedAt = data.finishedAt;

    for (int row = 0; row < _model->rowCount() && row < _lines.size(); ++row) {
        auto *preview = qobject_cast<QLineEdit *>(_model->cellWidget(row, ColPreview));
        if (!preview)
            continue;

        PostInfoTemplate::SheetLine const &line = _lines.at(row);
        if (line.kind == PostInfoTemplate::SheetLine::Kind::Comment) {
            preview->clear();
            preview->setPlaceholderText(tr("(not written)"));
            continue;
        }

        QString const source =
            line.kind == PostInfoTemplate::SheetLine::Kind::Field ? line.expression : line.raw;
        QString const rendered = PostInfoTemplate::render(
            source, data, false, PostInfoTemplate::OnUnknown::KeepVerbatim, nullptr, false, _escape);

        preview->setText(rendered);
        preview->setCursorPosition(0);
        preview->setPlaceholderText(_onlyKnownAfterPost(source) ? tr("filled in after the post")
                                                                : QString());
    }
}

bool PostInfoDialog::_onlyKnownAfterPost(const QString &expression) const
{
    for (PostInfoTemplate::Token const &token : PostInfoTemplate::tokensIn(expression)) {
        if (token.isMeta() || token.name == QLatin1String("date")
            || token.name == QLatin1String("dateStart"))
            continue;
        QString const placeholder = QStringLiteral("__%1__").arg(token.name);
        for (PostInfoTemplate::FieldDoc const &field : PostInfoTemplate::fields()) {
            if (placeholder == QLatin1String(field.placeholder) && !field.knownBeforePost)
                return true;
        }
    }
    return false;
}

void PostInfoDialog::onSaveModelAs()
{
    QString const suggested = _effectiveTemplatePath();
    QString const path      = QFileDialog::getSaveFileName(
        this,
        tr("Save the model as"),
        suggested,
        tr("Text files (*.txt *.tpl);;All files (*)"));
    if (path.isEmpty())
        return;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("Post information"), tr("Cannot write %1").arg(path));
        return;
    }
    file.write(PostInfoTemplate::buildTemplate(_lines, _crlf).toUtf8());
    if (!file.commit()) {
        QMessageBox::warning(this, tr("Post information"), tr("Cannot write %1").arg(path));
        return;
    }

    // Saved models join the list of the session, and the post switches to the
    // one just written: what you see is then what will be used.
    _setModelDirty(false);
    _rememberTemplate(path);
    _templateHint->setText(tr("Model in use: %1").arg(path));
}

void PostInfoDialog::_setModelDirty(bool dirty)
{
    _modelDirty = dirty;
    _saveAsButton->setText(dirty ? tr("Save as\342\200\246 *") : tr("Save as\342\200\246"));
}

// ======================================================================
//  your fields: the values of THIS post
// ======================================================================

//! Every __meta:name__ the model uses gets a row, wherever it appears: alone on
//! a line, or in the middle of a sentence like "me __originalName__ and more".
void PostInfoDialog::_syncFieldsFromModel()
{
    QStringList wanted;
    for (PostInfoTemplate::SheetLine const &line : _lines) {
        if (line.kind != PostInfoTemplate::SheetLine::Kind::Field)
            continue;
        for (PostInfoTemplate::Token const &token : PostInfoTemplate::tokensIn(line.expression)) {
            if (token.isMeta() && !token.arg.isEmpty() && !wanted.contains(token.arg))
                wanted << token.arg;
        }
    }

    QStringList existing;
    for (int row = 0; row < _fields->rowCount(); ++row) {
        if (auto *edit = qobject_cast<QLineEdit *>(_fields->cellWidget(row, ColName)))
            existing << edit->text().trimmed();
    }

    // Only the missing ones are added: a value already typed for a field this
    // model does not ask for is kept rather than thrown away.
    for (QString const &name : wanted) {
        if (!existing.contains(name))
            _addField(name, QString(), false);
    }
}

void PostInfoDialog::onAddField()
{
    // A field of yours is only useful if a line writes it, so both are created:
    // the value row here, and the line that carries it in the model.
    PostInfoTemplate::SheetLine line;
    line.kind       = PostInfoTemplate::SheetLine::Kind::Field;
    line.separator  = QStringLiteral(" =");
    line.expression = QStringLiteral("__meta:__");
    _lines << line;
    _setModelDirty(true);
    _fillModelTable();

    _addField(QString(), QString(), false);
    _refreshPreviews();

    int const row = _fields->rowCount() - 1;
    _fields->setCurrentCell(row, ColName);
    if (auto *edit = qobject_cast<QLineEdit *>(_fields->cellWidget(row, ColName)))
        edit->setFocus();
}

void PostInfoDialog::_addField(const QString &name, const QString &value, bool publish)
{
    int const row = _fields->rowCount();
    _fields->insertRow(row);

    QLineEdit *nameEdit = new QLineEdit(name, _fields);
    nameEdit->setObjectName(cellName("postInfoFieldName", row));
    nameEdit->setPlaceholderText(tr("album"));
    connect(nameEdit, &QLineEdit::textEdited, this, [this]() { _refreshPreviews(); });
    _fields->setCellWidget(row, ColName, nameEdit);

    QLineEdit *valueEdit = new QLineEdit(value, _fields);
    valueEdit->setObjectName(cellName("postInfoFieldValue", row));
    connect(valueEdit, &QLineEdit::textEdited, this, [this]() { _refreshPreviews(); });
    _fields->setCellWidget(row, ColValue, valueEdit);

    auto *publishCell = new CheckBoxCenterWidget(_fields, publish);
    publishCell->setObjectName(cellName("postInfoFieldNzb", row));
    _fields->setCellWidget(row, ColNzb, publishCell);

    QPushButton *del = new QPushButton(QString(QChar(0x2715)), _fields);
    del->setObjectName(cellName("postInfoFieldDel", row));
    del->setFixedWidth(30);
    del->setToolTip(tr("Remove this field. The line that uses it stays in the model."));
    connect(del, &QAbstractButton::clicked, this, [this, del]() {
        for (int i = 0; i < _fields->rowCount(); ++i) {
            if (_fields->cellWidget(i, ColDelField) == del) {
                _fields->removeRow(i);
                break;
            }
        }
        _refreshPreviews();
    });
    _fields->setCellWidget(row, ColDelField, del);
}

QMap<QString, MetaValue> PostInfoDialog::meta(QString *duplicate) const
{
    QMap<QString, MetaValue> meta;
    for (int row = 0; row < _fields->rowCount(); ++row) {
        auto *nameEdit  = qobject_cast<QLineEdit *>(_fields->cellWidget(row, ColName));
        auto *valueEdit = qobject_cast<QLineEdit *>(_fields->cellWidget(row, ColValue));
        if (!nameEdit || !valueEdit)
            continue;

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
        if (auto *cell = qobject_cast<CheckBoxCenterWidget *>(_fields->cellWidget(row, ColNzb)))
            publish = cell->isChecked();

        meta.insert(name, MetaValue(valueEdit->text(), publish ? MetaScope::Nzb : MetaScope::Local));
    }
    return meta;
}

// ======================================================================
//  the way out
// ======================================================================

void PostInfoDialog::onAccept()
{
    if (_modelDirty) {
        // The post uses the file, not this table. Leaving without saying so
        // would show one sheet here and write another one on disk.
        QMessageBox::StandardButton const answer = QMessageBox::question(
            this,
            tr("Post information"),
            tr("The model has been changed here, but not written to a file.\n\n"
               "The post uses the file, so these changes would be lost.\n"
               "Save the model now?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);

        if (answer == QMessageBox::Cancel)
            return;
        if (answer == QMessageBox::Save) {
            onSaveModelAs();
            if (_modelDirty)
                return; // the save was cancelled or failed: stay here
        }
    }
    accept();
}

//! The reference of every variable, built from the engine table so it cannot
//! drift from what the renderer actually supports.
void PostInfoDialog::onShowHelp()
{
    QString html = QStringLiteral("<h3>%1</h3><p>%2</p>")
                       .arg(tr("What can I put in a model"),
                            tr("A model is a plain text file. Write it as you want the sheet to "
                               "read, and put a variable wherever ngPost should fill something "
                               "in. Anything ngPost does not recognise is copied as it is, and a "
                               "line starting with # is a comment that is never written."));

    html += QStringLiteral("<pre>%1</pre>")
                .arg(QStringLiteral("# this line is a comment\n"
                                    "name    =__originalName__\n"
                                    "size    =__postSize__\n"
                                    "title   =__meta:title__\n"
                                    "comment =mine, __originalName__, and more text")
                         .toHtmlEscaped());

    html += QStringLiteral("<h4>%1</h4><p>%2</p>")
                .arg(tr("Mixing text and variables"),
                     tr("A line is free text: <b>comment =mine, __originalName__, and more</b> "
                        "writes the sentence with the name in the middle. You can mix as many "
                        "variables and as much text as you like on one line."));

    html += QStringLiteral("<h4>%1</h4><p>%2</p>")
                .arg(tr("Your own fields"),
                     tr("<b>__meta:name__</b> is a field of yours. Every name you use appears in "
                        "<i>Your fields</i> at the bottom of the window, waiting for a value. It "
                        "always goes into the post info file; tick <i>Also in NZB</i> to publish "
                        "it in the nzb as well."));

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
                        "prepare it, so the preview column shows it. <i>after the post</i> means "
                        "it only exists once the post is over, which is why it is blank here."));

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

    help.resize(760, 600);
    help.exec();
}
