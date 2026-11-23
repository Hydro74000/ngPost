// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Small SQL helpers shared by the parts of PostHistoryStore that are split
// over several files (queue manager, recoverable preparation). Internal to
// src/history.
//
//========================================================================

#ifndef HISTORY_POSTHISTORYSQL_H
#define HISTORY_POSTHISTORYSQL_H

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>
#include <QVariantList>

namespace historysql
{

//! A NULL that binds as a string column.
inline QVariant nullString()
{
    return QVariant(QMetaType::fromType<QString>());
}

//! Runs one statement with positional values; false sets @p error.
inline bool run(QSqlDatabase &db,
                const QString &sql,
                const QVariantList &values,
                QString *error,
                int *rows = nullptr)
{
    QSqlQuery q(db);
    q.prepare(sql);
    for (const QVariant &value : values)
        q.addBindValue(value);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    if (rows)
        *rows = q.numRowsAffected();
    return true;
}

} // namespace historysql

#endif // HISTORY_POSTHISTORYSQL_H
