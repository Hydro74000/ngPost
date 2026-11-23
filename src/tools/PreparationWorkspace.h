// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Where a set of archives is prepared (plan_queue.md §6.3, lot C5): one folder
// per post and generation under the effective temporary root, archives and
// sources moved aside kept apart, and a marker whose native identity is
// recorded. Nothing there is renamed back or deleted unless it is proven to
// be that set's: a folder that may be someone else's stays as it is.
//
//========================================================================

#ifndef TOOLS_PREPARATIONWORKSPACE_H
#define TOOLS_PREPARATIONWORKSPACE_H

#include <QString>

#include <optional>

struct NativeFileIdentity;

struct PreparationWorkspace
{
    QString root;       //!< canonical effective root (TMP_DIR, or TMP_RAM)
    QString setDir;     //!< <root>/ngpost-<post>-g<generation>
    QString archiveDir; //!< <setDir>/archives: what the tools write
    QString stagingDir; //!< <setDir>/staging: sources moved aside, never deleted with the archives
    QString markerPath; //!< <setDir>/.ngpost-set, never posted

    enum class Status {
        Ours,     //!< the folder and its marker are the ones recorded
        Missing,  //!< the whole set folder is gone (a temporary folder emptied)
        Ambiguous //!< present, but not provably ours: nothing is touched
    };

    //! Where set @p generation of post @p postId goes. None when @p root is not
    //! an existing absolute folder.
    static std::optional<PreparationWorkspace> plan(const QString &root,
                                                    qint64 postId,
                                                    int generation,
                                                    QString *error = nullptr);
    //! Creates the folders and the marker naming @p setId. A set folder that
    //! already exists is a collision: refused, left untouched.
    bool create(const QString &setId, NativeFileIdentity *marker, QString *error = nullptr) const;
    //! Whether the folder is still the one recorded for @p setId.
    Status verify(const QString &setId,
                  const QString &markerIdentityJson,
                  QString *why = nullptr) const;
    //! Deletes the archives of a set proven ours that keepRar does not retain.
    //! The staging folder goes only once empty: it may hold sources.
    bool removeArchives(const QString &setId,
                        const QString &markerIdentityJson,
                        bool retained,
                        QString *error = nullptr) const;
    //! @p path lies under the root, the root itself excluded, with no link on
    //! the way out of it.
    bool containsStrictly(const QString &path) const;
};

#endif // TOOLS_PREPARATIONWORKSPACE_H
