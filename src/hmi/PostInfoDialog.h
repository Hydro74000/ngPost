// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Per post editor for the record sheet: which model to use, and what to
// fill it with.
//
//========================================================================

#ifndef POSTINFODIALOG_H
#define POSTINFODIALOG_H

#include "postinfo/PostInfoData.h"
#include "postinfo/PostInfoTemplate.h"

#include <QDialog>
#include <QMap>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

class PostInfoDialog : public QDialog
{
    Q_OBJECT

public:
    //! \a configuredTemplate is what ngPost.conf provides, used when the post
    //! does not pick one of its own. \a sessionTemplates are the ones already
    //! opened during this run, offered again here.
    PostInfoDialog(const QString &configuredTemplate,
                   const QString &templateOverride,
                   const QMap<QString, MetaValue> &meta,
                   const QStringList &sessionTemplates = QStringList(),
                   const PostInfoData &preview = PostInfoData(),
                   QWidget *parent = nullptr);

    //! Models the dialog ended up offering, minus the ones the user dropped.
    QStringList sessionTemplates() const { return _sessionTemplates; }

    //! Empty when the post uses the model from the configuration.
    QString templateOverride() const;
    //! True when the user asked for the selected model to become the one the
    //! configuration provides from now on.
    bool setAsDefault() const;
    //! Names typed twice are reported instead of being silently collapsed.
    QMap<QString, MetaValue> meta(QString *duplicate = nullptr) const;

private slots:
    void onTemplateChosen(int index);
    void onForgetTemplate();
    void onAddField();
    //! Reads the model and lays out every line it will produce.
    void onLoadFieldsFromTemplate();
    //! Opens the reference of every __variable__, built from the engine table.
    void onShowHelp();

private:
    void _addField(const QString &name, const QString &value, bool publish);
    //! A line ngPost fills in by itself: shown, never edited, never deleted.
    void _addAutoField(const QString &placeholder,
                       const QString &value,
                       const QString &description);
    //! Value to show for a variable ngPost fills in, empty when it will only
    //! exist once the post is over.
    QString _previewValue(const PostInfoTemplate::Token &token,
                          const QMap<QString, QString> &values) const;
    QString _effectiveTemplatePath() const;

    QString      _configuredTemplate;
    PostInfoData _preview;

    void _fillTemplateList(const QString &override);
    void _rememberTemplate(const QString &path);
    void _updateForgetButton();

    QStringList  _sessionTemplates;
    bool         _fillingList = false;

    QComboBox    *_templateList;
    QPushButton  *_forgetButton;
    QLabel       *_templateHint;
    QCheckBox    *_setAsDefault;
    QPushButton  *_loadFieldsButton;
    QPushButton  *_helpButton;
    QTableWidget *_fields;
    QPushButton  *_addButton;
};

#endif // POSTINFODIALOG_H
