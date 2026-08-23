// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Per post editor for the record sheet.
//
// Two things live here, and they are deliberately not mixed: the MODEL, which
// is a file on disk shared by every post that uses it, and YOUR FIELDS, which
// are the values of this post alone. Editing a label belongs to the first,
// typing a title belongs to the second, and only the first is ever written
// back to a .txt.
//
//========================================================================

#ifndef POSTINFODIALOG_H
#define POSTINFODIALOG_H

#include "postinfo/PostInfoData.h"
#include "postinfo/PostInfoTemplate.h"

#include <QDialog>
#include <QMap>
#include <QString>
#include <QVector>

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
    PostInfoDialog(const QString                  &configuredTemplate,
                   const QString                  &templateOverride,
                   const QMap<QString, MetaValue> &meta,
                   const QStringList              &sessionTemplates = QStringList(),
                   const PostInfoData             &preview          = PostInfoData(),
                   QWidget                        *parent           = nullptr);

    //! Models the dialog ended up offering, minus the ones the user dropped,
    //! plus any it saved.
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
    //! Re-reads the model from disk, dropping any unsaved change to it.
    void onReloadModel();
    //! Opens the reference of every __variable__, built from the engine table.
    void onShowHelp();

    void onAddModelLine();
    void onAddField();
    void onSaveModelAs();

    void onAccept();

private:
    // ---- the model, i.e. the file ---------------------------------------
    void _loadModel();
    void _fillModelTable();
    void _removeModelLine(int lineIndex);
    //! Re-renders the preview column from the current lines and values.
    void _refreshPreviews();
    //! True when a line only exists once the post is over, so an empty preview
    //! means "not yet" rather than "you left it blank".
    bool _onlyKnownAfterPost(const QString &expression) const;

    // ---- your fields, i.e. this post ------------------------------------
    //! One value row per __meta:name__ the model uses, keeping what is typed.
    void _syncFieldsFromModel();
    void _addField(const QString &name, const QString &value, bool publish);

    // ---- the model list --------------------------------------------------
    QString _effectiveTemplatePath() const;
    void    _fillTemplateList(const QString &selected);
    void    _rememberTemplate(const QString &path);
    void    _updateForgetButton();

    void _setModelDirty(bool dirty);

    QString      _configuredTemplate;
    PostInfoData _preview;
    QStringList  _sessionTemplates;

    QVector<PostInfoTemplate::SheetLine> _lines;
    bool                                 _crlf        = false;
    //! Declared by the model itself, so the preview escapes exactly like the
    //! file will.
    PostInfoTemplate::Escape             _escape      = PostInfoTemplate::Escape::None;
    bool                                 _modelDirty  = false;
    bool                                 _fillingList = false;
    bool                                 _building    = false;

    QComboBox    *_templateList;
    QPushButton  *_forgetButton;
    QPushButton  *_reloadButton;
    QCheckBox    *_setAsDefault;
    QLabel       *_templateHint;

    QTableWidget *_model;
    QPushButton  *_addLineButton;
    QPushButton  *_saveAsButton;

    QTableWidget *_fields;
    QPushButton  *_addFieldButton;

    QPushButton  *_helpButton;
};

#endif // POSTINFODIALOG_H
