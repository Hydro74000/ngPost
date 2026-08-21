// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>

#include "PostInfoDialog.h"

#include "CheckBoxCenterWidget.h"
#include "postinfo/PostInfoTemplate.h"

#include <QCheckBox>
#include <QComboBox>
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
                               QWidget *parent)
    : QDialog(parent), _configuredTemplate(configuredTemplate)
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
    _fields = new QTableWidget(this);
    _fields->setObjectName(QStringLiteral("postInfoFieldsTable"));
    _fields->setColumnCount(4);
    _fields->verticalHeader()->hide();
    _fields->setHorizontalHeaderLabels(
        QStringList{ tr("Name"), tr("Value"), tr("NZB"), QString() });
    _fields->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    _fields->setColumnWidth(2, 60);
    _fields->setColumnWidth(3, 30);
    root->addWidget(_fields, 1);

    _addButton = new QPushButton(tr("Add a field"), this);
    _addButton->setObjectName(QStringLiteral("postInfoAddFieldButton"));
    root->addWidget(_addButton, 0, Qt::AlignLeft);

    root->addWidget(new QLabel(tr("The NZB box publishes a field inside the nzb, which "
                                  "circulates. Leave it off to keep it in your file only."),
                               this));

    QDialogButtonBox *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(_addButton, &QAbstractButton::clicked, this, &PostInfoDialog::onAddField);
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

//! The configured model first, marked as such, then anything else this post
//! already pointed at, then a way out to the filesystem.
void PostInfoDialog::_fillTemplateList(const QString &override)
{
    _templateList->clear();
    if (!_configuredTemplate.isEmpty())
        _templateList->addItem(tr("%1  (default)").arg(QFileInfo(_configuredTemplate).fileName()),
                               _configuredTemplate);
    else
        _templateList->addItem(tr("(no model yet)"), QString());

    if (!override.isEmpty() && override != _configuredTemplate)
        _templateList->addItem(QFileInfo(override).fileName(), override);

    _templateList->addItem(tr("Choose a file\342\200\246"), QStringLiteral("__browse__"));
    _templateList->setCurrentIndex(override.isEmpty() || override == _configuredTemplate ? 0 : 1);
}

void PostInfoDialog::_rememberTemplate(const QString &path)
{
    // Insert before the "Choose a file..." entry, and select it.
    int const browseIdx = _templateList->count() - 1;
    for (int i = 0; i < browseIdx; ++i) {
        if (_templateList->itemData(i).toString() == path) {
            _templateList->setCurrentIndex(i);
            return;
        }
    }
    _templateList->insertItem(browseIdx, QFileInfo(path).fileName(), path);
    _templateList->setCurrentIndex(browseIdx);
}

void PostInfoDialog::onTemplateChosen(int index)
{
    if (_templateList->itemData(index).toString() != QLatin1String("__browse__")) {
        onLoadFieldsFromTemplate();
        return;
    }

    QString const path = QFileDialog::getOpenFileName(
        this, tr("Post info model"), QString(), tr("Text files (*.txt *.tpl);;All files (*)"));
    if (path.isEmpty()) {
        _templateList->setCurrentIndex(0); // back to what it was
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
    QStringList const names = PostInfoTemplate::metaNamesIn(QString::fromUtf8(file.readAll()));
    file.close();

    // Existing rows are kept: a value already typed must survive a change of
    // model, only the missing names are added.
    QStringList existing;
    for (int row = 0; row < _fields->rowCount(); ++row)
        if (auto *edit = qobject_cast<QLineEdit *>(_fields->cellWidget(row, 0)))
            existing << edit->text().trimmed();

    int added = 0;
    for (QString const &name : names) {
        if (existing.contains(name))
            continue;
        _addField(name, QString(), false);
        ++added;
    }

    if (names.isEmpty())
        _templateHint->setText(tr("Model in use: %1 (it asks for no field of yours)").arg(path));
    else
        _templateHint->setText(
            tr("Model in use: %1 — %n field(s) added", "", added).arg(path));
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

    QPushButton *del = new QPushButton(QStringLiteral("X"), _fields);
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

QMap<QString, MetaValue> PostInfoDialog::meta(QString *duplicate) const
{
    QMap<QString, MetaValue> meta;
    for (int row = 0; row < _fields->rowCount(); ++row) {
        auto *nameEdit  = qobject_cast<QLineEdit *>(_fields->cellWidget(row, 0));
        auto *valueEdit = qobject_cast<QLineEdit *>(_fields->cellWidget(row, 1));
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
        if (auto *cell = qobject_cast<CheckBoxCenterWidget *>(_fields->cellWidget(row, 2)))
            publish = cell->isChecked();

        meta.insert(name, MetaValue(valueEdit->text(), publish ? MetaScope::Nzb : MetaScope::Local));
    }
    return meta;
}
