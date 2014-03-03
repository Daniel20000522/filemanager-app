/*
 * Copyright (C) 2012 Robin Burchell <robin+nemo@viroteck.net>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * "Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Nemo Mobile nor the names of its contributors
 *     may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
 */

#include "dirselection.h"
#include "dirmodel.h"
#include "iorequest.h"
#include "ioworkerthread.h"
#include "filesystemaction.h"
#include "externalfswatcher.h"
#include "clipboard.h"
#include "fmutil.h"


#ifndef DO_NOT_USE_TAG_LIB
#include <taglib/attachedpictureframe.h>
#include <taglib/id3v2tag.h>
#include <taglib/fileref.h>
#include <taglib/mpegfile.h>
#include <taglib/tag.h>
#include <taglib/audioproperties.h>
#endif

#include <errno.h>
#include <string.h>
#include <QDirIterator>
#include <QDir>
#include <QDebug>
#include <QFileIconProvider>
#include <QUrl>
#include <QDesktopServices>
#include <QMetaType>
#include <QDateTime>
#include <QMimeType>

#if defined(REGRESSION_TEST_FOLDERLISTMODEL)
# include <QColor>
# include <QBrush>
#endif





#define IS_VALID_ROW(row)             (row >=0 && row < mDirectoryContents.count())
#define WARN_ROW_OUT_OF_RANGE(row)    qWarning() << Q_FUNC_INFO << this << "row:" << row << "Out of bounds access"

#define IS_FILE_MANAGER_IDLE()            (!mAwaitingResults)


Q_GLOBAL_STATIC(IOWorkerThread, ioWorkerThread)

namespace {
    QHash<QByteArray, int> roleMapping;
}



/*!
 *  Sort was originally done in \ref onItemsAdded() and that code is now in \ref addItem(),
 *  the reason to keep doing sort and do not let QDir does it is that when adding new items
 *  by \ref mkdir() or \paste() it is not necessary to call refresh() to load the entire directory
 *  to organize it items again. New items order/position are organized by \ref addItem()
 *
 */
static CompareFunction availableCompareFunctions[2][2] =
{
    {fileCompareAscending, fileCompareDescending}
   ,{dateCompareAscending, dateCompareDescending}
};


DirModel::DirModel(QObject *parent)
    : DirItemAbstractListModel(parent)
    , mFilterDirectories(false)
    , mShowDirectories(true)
    , mAwaitingResults(false)
    , mIsRecursive(false)
    , mReadsMediaMetadata(false)
    , mShowHiddenFiles(false)
    , mSortBy(SortByName)
    , mSortOrder(SortAscending)
    , mCompareFunction(0)  
    , mExtFSWatcher(0)
    , mClipboard(new Clipboard(this))    
    , m_fsAction(new FileSystemAction(this) )    
{
    mNameFilters = QStringList() << "*";

    mSelection   =  new DirSelection(this, &mDirectoryContents);

    connect(m_fsAction, SIGNAL(progress(int,int,int)),
            this,     SIGNAL(progress(int,int,int)));

    connect(m_fsAction, SIGNAL(added(DirItemInfo)),
            this,     SLOT(onItemAdded(DirItemInfo)));

    connect(m_fsAction, SIGNAL(added(QString)),
            this,     SLOT(onItemAdded(QString)));

    connect(m_fsAction, SIGNAL(removed(DirItemInfo)),
            this,     SLOT(onItemRemoved(DirItemInfo)));

    connect(m_fsAction, SIGNAL(removed(QString)),
            this,     SLOT(onItemRemoved(QString)));  

    connect(m_fsAction, SIGNAL(error(QString,QString)),
            this,     SIGNAL(error(QString,QString)));

    connect(this,     SIGNAL(pathChanged(QString)),
            m_fsAction, SLOT(pathChanged(QString)));

    connect(mClipboard, SIGNAL(clipboardChanged()),
            this,       SIGNAL(clipboardChanged()));

    connect(m_fsAction,  SIGNAL(changed(DirItemInfo)),
           this,        SLOT(onItemChanged(DirItemInfo)));

    connect(mClipboard, SIGNAL(clipboardChanged()),
            m_fsAction, SLOT(onClipboardChanged()));

    connect(m_fsAction, SIGNAL(recopy(QStringList,QString)),
            mClipboard, SLOT(copy(QStringList,QString)));

    setCompareAndReorder();

    if (QIcon::themeName().isEmpty() && !FMUtil::hasTriedThemeName())
    {
        FMUtil::setThemeName();
    }
}



DirModel::~DirModel()
{
    stoptExternalFsWatcher();
}



QHash<int, QByteArray> DirModel::roleNames() const
{
    static QHash<int, QByteArray> roles;
    if (roles.isEmpty()) {
        roles = buildRoleNames();
    }

    return roles;
}




QHash<int, QByteArray> DirModel::buildRoleNames() const
{
    QHash<int, QByteArray> roles;
        roles.insert(FileNameRole, QByteArray("fileName"));
        roles.insert(AccessedDateRole, QByteArray("accessedDate"));
        roles.insert(CreationDateRole, QByteArray("creationDate"));
        roles.insert(ModifiedDateRole, QByteArray("modifiedDate"));
        roles.insert(FileSizeRole, QByteArray("fileSize"));
        roles.insert(IconSourceRole, QByteArray("iconSource"));
        roles.insert(FilePathRole, QByteArray("filePath"));
        roles.insert(IsDirRole, QByteArray("isDir"));
        roles.insert(IsFileRole, QByteArray("isFile"));
        roles.insert(IsReadableRole, QByteArray("isReadable"));
        roles.insert(IsWritableRole, QByteArray("isWritable"));
        roles.insert(IsExecutableRole, QByteArray("isExecutable"));
        roles.insert(IsSelectedRole, QByteArray("isSelected"));
        roles.insert(TrackTitleRole, QByteArray("trackTitle"));
        roles.insert(TrackArtistRole, QByteArray("trackArtist"));
        roles.insert(TrackAlbumRole, QByteArray("trackAlbum"));
        roles.insert(TrackYearRole, QByteArray("trackYear"));
        roles.insert(TrackNumberRole, QByteArray("trackNumber"));
        roles.insert(TrackGenreRole, QByteArray("trackGenre"));
        roles.insert(TrackLengthRole, QByteArray("trackLength"));
        roles.insert(TrackCoverRole, QByteArray("trackCover"));

    // populate reverse mapping
    if (roleMapping.isEmpty()) {
        QHash<int, QByteArray>::ConstIterator it = roles.constBegin();
        for (;it != roles.constEnd(); ++it)
            roleMapping.insert(it.value(), it.key());

        // make sure we cover all roles
        //    Q_ASSERT(roles.count() == IsFileRole - FileNameRole);
    }

    return roles;
}

QVariant DirModel::data(int row, const QByteArray &stringRole) const
{
    QHash<QByteArray, int>::ConstIterator it = roleMapping.constFind(stringRole);

    if (it == roleMapping.constEnd())
        return QVariant();

    return data(index(row, 0), *it);
}

QVariant DirModel::data(const QModelIndex &index, int role) const
{
//its not for QML
#if defined(REGRESSION_TEST_FOLDERLISTMODEL)
    if (!index.isValid() ||
        (role != Qt::DisplayRole && role != Qt::DecorationRole && role != Qt::BackgroundRole)
       )
    {
        return QVariant();
    }
    if (role == Qt::DecorationRole && index.column() == 0)
    {             
        QIcon icon;      
        QMimeType mime = mDirectoryContents.at(index.row()).mimeType();
        if (mime.isValid())
        {
            if (QIcon::hasThemeIcon(mime.iconName()) ) {
               icon = QIcon::fromTheme(mime.iconName());
            }
            else if (QIcon::hasThemeIcon(mime.genericIconName())) {
               icon = QIcon::fromTheme(mime.genericIconName());
            }
        }
        if (icon.isNull())
        {
            if (mDirectoryContents.at(index.row()).isLocal())
            {
                icon =  QFileIconProvider().icon(mDirectoryContents.at(index.row()).diskFileInfo());
            }
            else
            if (mDirectoryContents.at(index.row()).isDir())
            {
                icon =  QFileIconProvider().icon(QFileIconProvider::Folder);
            }
            else
            {
                icon =  QFileIconProvider().icon(QFileIconProvider::File);
            }
        }
        return icon;
    }
    if (role == Qt::BackgroundRole && index.column() == 0)
    {
        if (mDirectoryContents.at(index.row()).isSelected())
        {
           //TODO it'd better to get some style or other default
           //     background color
           return QBrush(Qt::lightGray);
        }
        return QVariant();
    }
    role = FileNameRole + index.column();
#else
    if (role < FileNameRole || role > TrackCoverRole) {
        qWarning() << Q_FUNC_INFO << this << "Got an out of range role: " << role;
        return QVariant();
    }

    if (index.row() < 0 || index.row() >= mDirectoryContents.count()) {
        qWarning() << "Attempted to access out of range row: " << index.row();
        return QVariant();
    }

    if (index.column() != 0)
        return QVariant();
#endif

    const DirItemInfo &fi = mDirectoryContents.at(index.row());

    switch (role) {
        case FileNameRole:
            return fi.fileName();
        case AccessedDateRole:
            return fi.lastRead();
        case CreationDateRole:
            return fi.created();
        case ModifiedDateRole:
            return fi.lastModified();
        case FileSizeRole: {
             if (fi.isDir() && fi.isLocal())
             {
                return dirItems(fi.diskFileInfo());
             }
             return fileSize(fi.size());
        }
        case IconSourceRole: {
            const QString &fileName = fi.fileName();

            if (fi.isDir())
                return QLatin1String("image://theme/icon-m-common-directory");

            if (fileName.endsWith(QLatin1String(".jpg"), Qt::CaseInsensitive) ||
                fileName.endsWith(QLatin1String(".png"), Qt::CaseInsensitive)) {
                return QLatin1String("image://nemoThumbnail/") + fi.filePath();
            }

            return "image://theme/icon-m-content-document";
        }
        case FilePathRole:
            return fi.filePath();
        case IsDirRole:
            return fi.isDir();
        case IsFileRole:
            return !fi.isDir();
        case IsReadableRole:
            return fi.isReadable();
        case IsWritableRole:
            return fi.isWritable();
        case IsExecutableRole:
            return fi.isExecutable();
        case IsSelectedRole:
            return fi.isSelected();
#ifndef DO_NOT_USE_TAG_LIB
        case TrackTitleRole:
        case TrackArtistRole:
        case TrackAlbumRole:
        case TrackYearRole:
        case TrackNumberRole:
        case TrackGenreRole:
        case TrackLengthRole:
        case TrackCoverRole:
             if (mReadsMediaMetadata && fi.isLocal())
             {
                 return getAudioMetaData(fi.diskFileInfo(), role);
             }
             break;
#endif
        default:
#if !defined(REGRESSION_TEST_FOLDERLISTMODEL)
            // this should not happen, ever
            Q_ASSERT(false);
            qWarning() << Q_FUNC_INFO << this << "Got an unknown role: " << role;
#endif
            break;
    }

    return QVariant();
}

void DirModel::setPath(const QString &pathName)
{
    if (pathName.isEmpty())
        return;   

    if (!canReadDir(pathName))
    {
        emit error(tr("cannot read path"), pathName);
        return;
    }

    if (mAwaitingResults) {
        // TODO: handle the case where pathName != our current path, cancel old
        // request, start a new one
        qDebug() << Q_FUNC_INFO << this << "Ignoring path change request, request already running";
        return;
    }

    mAwaitingResults = true;
    emit awaitingResultsChanged();
#if DEBUG_MESSAGES
    qDebug() << Q_FUNC_INFO << this << "Changing to " << pathName << " on " << QThread::currentThreadId();
#endif

    clear();

    DirListWorker *dlw  = createWorkerRequest(IORequest::DirList, pathName);
    connect(dlw, SIGNAL(itemsAdded(DirItemInfoList)), SLOT(onItemsAdded(DirItemInfoList)));
    connect(dlw, SIGNAL(workerFinished()), SLOT(onResultsFetched()));
    ioWorkerThread()->addRequest(dlw);

    mCurrentDir = pathName;
    emit pathChanged(pathName);
}


void DirModel::onResultsFetched() {
    if (mAwaitingResults) {
#if DEBUG_MESSAGES
        qDebug() << Q_FUNC_INFO << this << "No longer awaiting results";
#endif
        mAwaitingResults = false;
        emit awaitingResultsChanged();
    }    
}

void DirModel::onItemsAdded(const DirItemInfoList &newFiles)
{
#if DEBUG_MESSAGES
    qDebug() << Q_FUNC_INFO << this << "Got new files: " << newFiles.count();
#endif

    if (newFiles.count() > 0)
    {
        mDirectoryContents.reserve(newFiles.count()) ;
    }
    foreach (const DirItemInfo &fi, newFiles) {

        bool doAdd = false;
        foreach (const QString &nameFilter, mNameFilters) {
            // TODO: using QRegExp for wildcard matching is slow
            QRegExp re(nameFilter, Qt::CaseInsensitive, QRegExp::Wildcard);
            if (re.exactMatch(fi.fileName()) || (fi.isDir() && !mFilterDirectories)) {
                doAdd = true;
                break;
            }
        }

        if (!doAdd)
            continue;

        addItem(fi);
    }
}

void DirModel::rm(const QStringList &paths)
{
   m_fsAction->remove(paths);
}


bool DirModel::rename(const QString& oldName, const QString &newName)
{
    return rename(getIndex(oldName), newName);
}


bool DirModel::rename(int row, const QString &newName)
{
#if DEBUG_MESSAGES
    qDebug() << Q_FUNC_INFO << this << "Renaming " << row << " to " << newName;
#endif

    if (!IS_VALID_ROW(row)) {
        WARN_ROW_OUT_OF_RANGE(row);
        return false;
    }

    const DirItemInfo &fi = mDirectoryContents.at(row);
    QString newFullFilename(fi.absolutePath() + QDir::separator() + newName);

    //QFile::rename() works for File and Dir
    QFile f(fi.absoluteFilePath());
    bool retval = f.rename(newFullFilename);
    if (!retval)
    {
        qDebug() << Q_FUNC_INFO << this << "Rename returned error code: " << f.error() << f.errorString();
        emit(QObject::tr("Rename error"), f.errorString());
    }
    else
    {
        bool isSelected =  mDirectoryContents.at(row).isSelected();
        onItemRemoved(mDirectoryContents.at(row));
        int newRow = addItem(DirItemInfo(QFileInfo(newFullFilename)));
        //keep previous selected state, selection takes care of everything
        mSelection->setIndex(newRow,isSelected);
    }
    return retval;
}

void DirModel::mkdir(const QString &newDir)
{
    QDir dir(mCurrentDir);
    bool retval = dir.mkdir(newDir);
    if (!retval) {
        const char *errorStr = strerror(errno);
        qDebug() << Q_FUNC_INFO << this << "Error creating new directory: " << errno << " (" << errorStr << ")";
        emit error(QObject::tr("Error creating new folder"), errorStr);
    } else {
        onItemAdded(dir.filePath(newDir));
    }
}

bool DirModel::showDirectories() const
{
    return mShowDirectories;
}

void DirModel::setShowDirectories(bool showDirectories)
{
    mShowDirectories = showDirectories;
    refresh();
    emit showDirectoriesChanged();
}

bool DirModel::isRecursive() const
{
    return mIsRecursive;
}

void DirModel::setIsRecursive(bool isRecursive)
{
    mIsRecursive = isRecursive;
    refresh();
    emit isRecursiveChanged();
}

bool DirModel::readsMediaMetadata() const
{
    return mReadsMediaMetadata;
}

void DirModel::setReadsMediaMetadata(bool readsMediaMetadata)
{
    mReadsMediaMetadata = readsMediaMetadata;
    refresh();
    emit readsMediaMetadataChanged();
}

bool DirModel::filterDirectories() const
{
    return mFilterDirectories;
}

void DirModel::setFilterDirectories(bool filterDirectories)
{
    mFilterDirectories = filterDirectories;
    refresh();
    emit filterDirectoriesChanged();
}

QStringList DirModel::nameFilters() const
{
    return mNameFilters;
}

void DirModel::setNameFilters(const QStringList &nameFilters)
{
    mNameFilters = nameFilters;
    refresh();
    emit nameFiltersChanged();
}

bool DirModel::awaitingResults() const
{
    return mAwaitingResults;
}


QString DirModel::parentPath() const
{
    QDir dir(mCurrentDir);
    if (dir.isRoot()) {
        qDebug() << Q_FUNC_INFO << this << "already at root";
        return mCurrentDir;
    }

    bool success = dir.cdUp();
    if (!success) {
        qWarning() << Q_FUNC_INFO << this << "Failed to to go to parent of " << mCurrentDir;
        return mCurrentDir;
    }
    qDebug() << Q_FUNC_INFO << this << "returning" << dir.absolutePath();
    return dir.absolutePath();
}

QString DirModel::homePath() const
{
    return QDir::homePath();
}

#if defined(REGRESSION_TEST_FOLDERLISTMODEL)
 QVariant  DirModel::headerData(int section, Qt::Orientation orientation, int role) const
 {
   if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
   {
       QVariant ret;
       QHash<int, QByteArray> roles = this->roleNames();
       section += FileNameRole;
       if (roles.contains(section))
       {
           QString header=  QString(roles.value(section));
           ret = header;
       }
       return ret;
   }
   return QAbstractItemModel::headerData(section, orientation, role);
 }
#endif


void DirModel::goHome()
{
    setPath(QDir::homePath());
}


bool DirModel::cdUp()
{
    int ret = false;
    if (!mCurrentDir.isEmpty()) // we are in any dir
    {
        QDir current(mCurrentDir);
        if (current.cdUp())
        {
            setPath(current.absolutePath());
            ret = true;
        }
    }
    return ret;
}


void DirModel::removeIndex(int row)
{
    if (IS_VALID_ROW(row))
    {
        const DirItemInfo &fi = mDirectoryContents.at(row);
        QStringList list(fi.absoluteFilePath());
        this->rm(list);
    }
    else
    {
        WARN_ROW_OUT_OF_RANGE(row);
    }
}

void DirModel::removePaths(const QStringList& items)
{
     this->rm(items);
}

void DirModel::copyIndex(int row)
{
    if (IS_VALID_ROW(row))
    {
        const DirItemInfo &fi = mDirectoryContents.at(row);
        QStringList list(fi.absoluteFilePath());
        this->copyPaths(list);
    }
    else
    {
        WARN_ROW_OUT_OF_RANGE(row);
    }
}

void DirModel::copyPaths(const QStringList &items)
{
   mClipboard->copy(items, mCurrentDir);
}


void DirModel::cutIndex(int row)
{
    if (IS_VALID_ROW(row))
    {
        const DirItemInfo &fi = mDirectoryContents.at(row);
        QStringList list(fi.absoluteFilePath());
        this->cutPaths(list);
    }
    else
    {
        WARN_ROW_OUT_OF_RANGE(row);
    }
}


void DirModel::cutPaths(const QStringList &items)
{
     mClipboard->cut(items, mCurrentDir);
}


void DirModel::paste()
{
    ClipboardOperation operation;
    QStringList items = mClipboard->paste(operation);
    if (operation == ClipboardCut)
    {
         m_fsAction->moveIntoCurrentPath(items);
    }
    else
    {
        m_fsAction->copyIntoCurrentPath(items);
    }   
}


bool  DirModel::cdIntoIndex(int row)
{
    bool ret = false;
    if (IS_VALID_ROW(row))
    {
        ret = cdInto(mDirectoryContents.at(row));
    }
    else
    {
        WARN_ROW_OUT_OF_RANGE(row);
    }
    return ret;
}


bool  DirModel::cdIntoPath(const QString &filename)
{
    bool ret = false;
    DirItemInfo fi(filename);
    if (fi.isValid())
    {
        if (fi.isRelative())
        {
            fi.setFile(mCurrentDir, filename);
        }
        ret =  cdInto(fi);
    }
    return ret;
}


bool  DirModel::cdInto(const DirItemInfo &fi)
{
    bool ret = false;
    if (canReadDir(fi.diskFileInfo()))
    {
        if (fi.isRelative())
        {
            QDir childDir(mCurrentDir);
            if ( childDir.cd(fi.fileName()) )
            {
                setPath(childDir.absolutePath());
                ret = true;
            }
        }
        else
        {
            ret = true;
            setPath(fi.absoluteFilePath());
        }
    }
    return ret;
}

/*!
 * \brief DirModel::onItemRemoved()
 * \param pathname full pathname of removed file
 */
void DirModel::onItemRemoved(const QString &pathname)
{
    DirItemInfo info(pathname);
    onItemRemoved(info);
}


void DirModel::onItemRemoved(const DirItemInfo &fi)
{  
    int row = rowOfItem(fi);
#if DEBUG_MESSAGES || DEBUG_EXT_FS_WATCHER
    qDebug() <<  Q_FUNC_INFO << this
             << "row" << row
             << "name" << fi.absoluteFilePath()
             << "removed[True|False]:" << (row >= 0);
#endif
    if (row >= 0)
    {
        beginRemoveRows(QModelIndex(), row, row);
        if (mDirectoryContents.at(row).isSelected())
        {
            mSelection->itemGoingToBeRemoved(mDirectoryContents.at(row));
        }
        mDirectoryContents.remove(row,1);
        endRemoveRows();
    }
}


/*!
 * \brief DirModel::onItemAdded()
 * \param pathname full pathname of the added file
 */
void DirModel::onItemAdded(const QString &pathname)
{
    DirItemInfo info(pathname);
    onItemAdded(info);
}


void DirModel::onItemAdded(const DirItemInfo &fi)
{
    int newRow = addItem(fi);
    emit insertedRow(newRow);
}

/*!
 * \brief DirModel::addItem() adds an item into the model
 *        This code was moved from onItemsAdded(const QVector<QFileInfo> &newFiles),
 *           the reason is:  this code now is used for \ref mkdir() and for \ref paste() operations
 *           that inserts new items
 * \param fi
 * \return  the index where it was inserted, it can be used in the view
 * \sa insertedRow()
 */
int DirModel::addItem(const DirItemInfo &fi)
{
    DirItemInfoList::Iterator it = qLowerBound(mDirectoryContents.begin(),
                                                  mDirectoryContents.end(),
                                                  fi,
                                                  mCompareFunction);
    int idx =  mDirectoryContents.count();

    if (it == mDirectoryContents.end()) {
        beginInsertRows(QModelIndex(), idx, idx);
        mDirectoryContents.append(fi);
        endInsertRows();
    } else {
        idx = it - mDirectoryContents.begin();
        beginInsertRows(QModelIndex(), idx, idx);
        mDirectoryContents.insert(it, fi);
        endInsertRows();
    }   
    return idx;
}

/*!
 * \brief DirModel::onItemChanged() Changes an item data
 *
 * \note If the item does  not exist it is inserted
 *
 * \param fi DirItemInfo of the item
 */
void DirModel::onItemChanged(const DirItemInfo &fi)
{
    int row = rowOfItem(fi);
    if (row >= 0)
    {       
        if (mDirectoryContents.at(row).isSelected())
        {
            mSelection->itemGoingToBeReplaced(mDirectoryContents.at(row), fi);
            DirItemInfo *myFi = const_cast<DirItemInfo*> (&fi);
            myFi->setSelection(true);
        }
        mDirectoryContents[row] = fi;
        notifyItemChanged(row);
    }
    else
    {   // it simplifies some logic outside, when removing and adding on the same operation
        onItemAdded(fi);
    }
}


void DirModel::cancelAction()
{
    m_fsAction->cancel();
}


QString DirModel::fileSize(qint64 size) const
{
    struct UnitSizes
    {
        qint64      bytes;
        const char *name;
    };

    static UnitSizes m_unitBytes[5] =
    {
        { 1,           "Bytes" }
       ,{1024,         "kB"}
        // got it from http://wiki.answers.com/Q/How_many_bytes_are_in_a_megabyte
       ,{1000 * 1000,  "MB"}
       ,{1000 *  m_unitBytes[2].bytes,   "GB"}
       ,{1000 *  m_unitBytes[3].bytes, "TB"}
    };

    QString ret;
    int unit = sizeof(m_unitBytes)/sizeof(m_unitBytes[0]);
    while( unit-- > 1 && size < m_unitBytes[unit].bytes );
    if (unit > 0 )
    {
        ret.sprintf("%0.1f %s", (float)size/m_unitBytes[unit].bytes,
                    m_unitBytes[unit].name);
    }
    else
    {
        ret.sprintf("%ld %s", (long int)size, m_unitBytes[0].name);
    }
    return ret;
}



bool DirModel::getShowHiddenFiles() const
{
    return mShowHiddenFiles;
}


void DirModel::setShowHiddenFiles(bool show)
{
    if (show != mShowHiddenFiles)
    {
        mShowHiddenFiles = show;
        refresh();
        emit showHiddenFilesChanged();
    }
}


void DirModel::toggleShowDirectories()
{
    setShowDirectories(!mShowDirectories);
}


void DirModel::toggleShowHiddenFiles()
{
    setShowHiddenFiles(!mShowHiddenFiles);
}


DirModel::SortBy
DirModel::getSortBy()  const
{
    return mSortBy;
}


void DirModel::setSortBy(SortBy field)
{
    if (field != mSortBy)
    {
        mSortBy = field;
        setCompareAndReorder();
        emit sortByChanged();
    }
}


DirModel::SortOrder
DirModel::getSortOrder() const
{
    return mSortOrder;
}

void DirModel::setSortOrder(SortOrder order)
{
    if ( order != mSortOrder )
    {
        mSortOrder = order;
        setCompareAndReorder();
        emit sortOrderChanged();
    }
}


void DirModel::toggleSortOrder()
{
    SortOrder  order = static_cast<SortOrder> (mSortOrder ^ 1);
    setSortOrder(order);
}


void DirModel::toggleSortBy()
{
    SortBy by = static_cast<SortBy> (mSortBy ^ 1);
    setSortBy(by);
}

/*!
 * \brief DirModel::setCompareAndReorder() called when  SortOrder or SortBy change
 *
 *  It does not reload items from disk, just reorganize items from \a mDirectoryContents array
 */
void DirModel::setCompareAndReorder()
{
    mCompareFunction = availableCompareFunctions[mSortBy][mSortOrder];
    if (mDirectoryContents.count() > 0 && !mAwaitingResults )
    {
        DirItemInfoList tmpDirectoryContents = mDirectoryContents;
        beginResetModel();
        mDirectoryContents.clear();
        endResetModel();
        for(int counter=0; counter < tmpDirectoryContents.count(); counter++)
        {
            addItem(tmpDirectoryContents.at(counter));
        }
    }
}


int DirModel::getClipboardUrlsCounter() const
{
    return mClipboard->clipboardLocalUrlsCounter();
}


int DirModel::rowOfItem(const DirItemInfo& fi)
{
    int row = -1;
    //to use qBinaryFind() the array needs to be ordered ascending
    if (mCompareFunction == fileCompareAscending)
    {
        DirItemInfoList::Iterator it = qBinaryFind(mDirectoryContents.begin(),
                                                      mDirectoryContents.end(),
                                                      fi,
                                                      fileCompareExists);
        if (it != mDirectoryContents.end())
        {
            row = it - mDirectoryContents.begin();
        }
    }
    else //walk through whole array
    {
       //TODO improve this search
       int counter = mDirectoryContents.count();
       while (counter--)
       {
           if ( 0 == QString::localeAwareCompare(fi.absoluteFilePath(),
                              mDirectoryContents.at(counter).absoluteFilePath()) )
           {
               row = counter;
               break;
           }
       }
    }
    return row;
}


QDir::Filter DirModel::currentDirFilter() const
{
    int filter = QDir::AllEntries | QDir::NoDotAndDotDot ;
    if (!mShowDirectories)
    {
        filter &= ~QDir::AllDirs;
        filter &= ~QDir::Dirs;
    }
    if (mShowHiddenFiles)
    {
        filter |= QDir::Hidden;
    }
    if (mIsRecursive)
    {
        filter |= QDir::NoSymLinks;
    }
    QDir::Filter dirFilter = static_cast<QDir::Filter>(filter);
    return dirFilter;
}

QString DirModel::dirItems(const DirItemInfo& fi) const
{
    int counter = 0;
    QDir d(fi.absoluteFilePath(), QString(), QDir::NoSort, currentDirFilter());
    counter = d.count();
    if (counter < 0)
    {
        counter = 0;
    }
    QString ret (QString::number(counter) + QLatin1Char(' '));
    ret += QObject::tr("items");
    return ret;
}


bool DirModel::openIndex(int row)
{
    bool ret = false;
    if (IS_VALID_ROW(row))
    {
        ret = openItem(mDirectoryContents.at(row));
    }
    else
    {
        WARN_ROW_OUT_OF_RANGE(row);
    }
    return ret;
}

bool DirModel::openPath(const QString &filename)
{
    DirItemInfo fi(setParentIfRelative(filename));
    return openItem(fi);
}

/*!
 * \brief DirModel::openItem() opens a directory/file
 * \param fi
 * \return  true it could open the item
 */
bool DirModel::openItem(const DirItemInfo &fi)
{
    bool ret = false;
    if (fi.isLocal())
    {
        if (canReadDir(fi.diskFileInfo()))
        {
            ret = cdInto(fi.diskFileInfo());
        }
        else
        {
            //TODO open executables
            if (canReadFile(fi.diskFileInfo()))
            {
                ret = QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absoluteFilePath()));
            }
        }
    }
    return ret;
}

/*!
 * \brief DirModel::createWorkerRequest() create a request for IORequestWorker
 * \param requestType the common IORequest::DirList to fill a directory content
 *                    or IORequest::DirAutoRefresh that will verify any external File System modification
 * \param pathName  the path to get content
 * \return          the thread object
 */
DirListWorker * DirModel::createWorkerRequest(IORequest::RequestType requestType,
                                              const QString& pathName)
{
    DirListWorker * reqThread = 0;
    QDir::Filter dirFilter = currentDirFilter();
    if (requestType == IORequest::DirList)
    {
        // TODO: we need to set a spinner active before we start getting results from DirListWorker
        reqThread = new DirListWorker(pathName, dirFilter, mIsRecursive);
    }
    else
    {
        reqThread = new ExternalFileSystemChangesWorker(mDirectoryContents,
                                                  pathName,
                                                  dirFilter, mIsRecursive);
    }  
    return reqThread;
}



/*!
 * \brief DirModel::startExternalFsWatcher() starts the External File System Watcher
 */
void DirModel::startExternalFsWatcher()
{
#if DEBUG_EXT_FS_WATCHER
        qDebug() << "[extFsWorker]" << QDateTime::currentDateTime().toString("hh:mm:ss.zzz")
                  << Q_FUNC_INFO << this;

#endif
    if (!mExtFSWatcher)
    {
        mExtFSWatcher = new ExternalFSWatcher(this);
        mExtFSWatcher->setIntervalToNotifyChanges(EX_FS_WATCHER_TIMER_INTERVAL);
        connect(this,          SIGNAL(pathChanged(QString)),
                mExtFSWatcher, SLOT(setCurrentPath(QString)));

        connect(mExtFSWatcher, SIGNAL(pathModified()),
                this,          SLOT(onThereAreExternalChanges()));

       //setCurrentPath() checks for empty paths
       mExtFSWatcher->setCurrentPath(mCurrentDir);
    }
}



/*!
 * \brief DirModel::stoptExternalFsWatcher stops the External File System Watcher
 */
void DirModel::stoptExternalFsWatcher()
{
#if DEBUG_EXT_FS_WATCHER
        qDebug() << "[extFsWatcher]" << QDateTime::currentDateTime().toString("hh:mm:ss.zzz")
                  << Q_FUNC_INFO << this;
#endif
   if (mExtFSWatcher)
   {
       delete mExtFSWatcher;
       mExtFSWatcher = 0;
   }
}


void DirModel::onThereAreExternalChanges()
{
    if ( IS_FILE_MANAGER_IDLE() )
    {
#if DEBUG_EXT_FS_WATCHER
        qDebug() << "[extFsWatcher]" << QDateTime::currentDateTime().toString("hh:mm:ss.zzz")
                 << Q_FUNC_INFO << this << "File System modified";
#endif
        DirListWorker *w =
                createWorkerRequest(IORequest::DirListExternalFSChanges,
                                    mCurrentDir
                                   );
        ExternalFileSystemChangesWorker *extFsWorker =
                static_cast<ExternalFileSystemChangesWorker*> (w);

        connect(extFsWorker,    SIGNAL(added(DirItemInfo)),
                this,         SLOT(onItemAddedOutsideFm(DirItemInfo)));
        connect(extFsWorker,    SIGNAL(removed(DirItemInfo)),
                this,         SLOT(onItemRemovedOutSideFm(DirItemInfo)));
        connect(extFsWorker,    SIGNAL(changed(DirItemInfo)),
                this,         SLOT(onItemChangedOutSideFm(DirItemInfo)));
        connect(extFsWorker,    SIGNAL(finished(int)),
                this,         SLOT(onExternalFsWorkerFinished(int)));

        ioWorkerThread()->addRequest(extFsWorker);
    }
#if DEBUG_EXT_FS_WATCHER
    else
    {
        qDebug() << "[extFsWatcher]" << QDateTime::currentDateTime().toString("hh:mm:ss.zzz")
                  << Q_FUNC_INFO << this << "Busy, nothing to do";
    }
#endif
}

/*!
 * \brief DirModel::onItemAddedOutsideFm() It receives a  signal saying an item was added by other application
 * \param fi
 */
void DirModel::onItemAddedOutsideFm(const DirItemInfo &fi)
{
#if DEBUG_EXT_FS_WATCHER
    int before  = rowCount();
#endif
    if (IS_FILE_MANAGER_IDLE())
    {
        int row = rowOfItem(fi);
        if (row == -1)
        {
            onItemAdded(fi);
        }
    }
#if DEBUG_EXT_FS_WATCHER
        qDebug() << "[extFsWatcher]" << QDateTime::currentDateTime().toString("hh:mm:ss.zzz")
                 << Q_FUNC_INFO << this
                 << "counterBefore:" << before
                 << "added" << fi.absoluteFilePath()
                 << "counterAfter:" << rowCount();
#endif
}

/*!
 * \brief DirModel::onItemRemovedOutSideFm() It receives a  signal saying an item was removed by other application
 *
 * Just calls \ref onItemRemoved() which already checks if the item exists
 *
 * \param fi
 */
void DirModel::onItemRemovedOutSideFm(const DirItemInfo &fi)
{
    if (IS_FILE_MANAGER_IDLE())
    {
#if DEBUG_EXT_FS_WATCHER
        qDebug() << "[extFsWatcher]" << QDateTime::currentDateTime().toString("hh:mm:ss.zzz")
                 << Q_FUNC_INFO << this << "removed" << fi.absoluteFilePath();
#endif
        onItemRemoved(fi);
    }
}

/*!
 * \brief DirModel::onItemChangedOutSideFm()
 *
 * A File or a Dir modified by other applications: size,date, permissions
 */
void DirModel::onItemChangedOutSideFm(const DirItemInfo &fi)
{   
    if (IS_FILE_MANAGER_IDLE())
    {       
        onItemChanged(fi);
#if DEBUG_EXT_FS_WATCHER
        qDebug() << "[extFsWatcher]" << QDateTime::currentDateTime().toString("hh:mm:ss.zzz")
                 << Q_FUNC_INFO << this << "changed" << fi.absoluteFilePath()
                 << "from row" << rowOfItem(fi);
#endif
    }
}


/*!
 * \brief DirModel::onExternalFsWatcherFinihed()
 */
void DirModel::onExternalFsWorkerFinished(int currentDirCounter)
{

#if DEBUG_EXT_FS_WATCHER
    qDebug() << "[extFsWatcher]" << QDateTime::currentDateTime().toString("hh:mm:ss.zzz")
             << Q_FUNC_INFO << this
             << "currentDirCounter:"  << currentDirCounter;

#endif
    if (currentDirCounter == 0 && IS_FILE_MANAGER_IDLE())
    {
        clear();
    }
}


/*!
 * \brief DirModel:getEnabledExternalFSWatcher()
 * \return true if the External File System Watcher is enabled
 */
bool DirModel::getEnabledExternalFSWatcher() const
{
   return mExtFSWatcher ? true : false;
}


/*!
 * \brief DirModel::setEnabledExternalFSWatcher() enable/disable External File Sysmte Watcher
 * \param enable
 */
void DirModel::setEnabledExternalFSWatcher(bool enable)
{
    if(enable)
    {
        startExternalFsWatcher();
    }
    else
    {
        stoptExternalFsWatcher();
    }
}


bool DirModel::existsDir(const QString &folderName) const
{
    DirItemInfo d(setParentIfRelative(folderName));
    return d.exists() && d.isDir();
}

bool  DirModel::canReadDir(const QString &folderName) const
{
    DirItemInfo d(setParentIfRelative(folderName));
    return canReadDir(d.diskFileInfo());
}

bool  DirModel::canReadDir(const QFileInfo & d) const
{
    return d.exists() && d.isDir() && d.isReadable() && d.isExecutable();
}

bool DirModel::existsFile(const QString &fileName) const
{
     DirItemInfo f(setParentIfRelative(fileName));
     return f.exists() && f.isFile();
}

bool DirModel::canReadFile(const QString &fileName) const
{
    DirItemInfo  f(setParentIfRelative(fileName));
    return canReadFile(f.diskFileInfo());
}

bool DirModel::canReadFile(const QFileInfo &f) const
{
    return f.exists() && f.isFile() && f.isReadable();
}



QDateTime DirModel::curPathCreatedDate() const
{
   QDateTime d;
   QFileInfo f(mCurrentDir);
   if (f.exists())
   {
       d = f.created();
   }
   return d;
}


QDateTime DirModel::curPathModifiedDate() const
{
    QDateTime d;
    QFileInfo f(mCurrentDir);
    if (f.exists())
    {
        d = f.lastModified();
    }
    return d;
}


QDateTime DirModel::curPathAccessedDate() const
{
    QDateTime d;
    QFileInfo f(mCurrentDir);
    if (f.exists())
    {
        d = f.lastRead();
    }
    return d;
}


bool  DirModel::curPathIsWritable() const
{
    QFileInfo f(mCurrentDir);
    return f.exists() && f.isWritable();
}

QString DirModel::curPathCreatedDateLocaleShort() const
{
    QString date;
    QDateTime d(curPathCreatedDate());
    if (!d.isNull())
    {
        date = d.toString(Qt::SystemLocaleShortDate);
    }
    return date;
}


QString DirModel::curPathModifiedDateLocaleShort() const
{
    QString date;
    QDateTime d(curPathModifiedDate());
    if (!d.isNull())
    {
        date = d.toString(Qt::SystemLocaleShortDate);
    }
    return date;
}


QString DirModel::curPathAccessedDateLocaleShort() const
{
    QString date;
    QDateTime d(curPathAccessedDate());
    if (!d.isNull())
    {
        date = d.toString(Qt::SystemLocaleShortDate);
    }
    return date;
}


QFileInfo  DirModel::setParentIfRelative(const QString &fileOrDir) const
{
    QFileInfo myFi(fileOrDir);
    if (myFi.isRelative())
    {
        myFi.setFile(mCurrentDir, fileOrDir);
        QFileInfo abs(myFi.absoluteFilePath());
        myFi = abs;
    }
    return myFi;
}


int DirModel::getProgressCounter() const
{
    return m_fsAction->getProgressCounter();
}


void DirModel::clear()
{
    beginResetModel();
    mDirectoryContents.clear();
    mSelection->clear();
    endResetModel();
}


DirSelection * DirModel::selectionObject() const
{
    return mSelection;
}


void DirModel::registerMetaTypes()
{
    qRegisterMetaType<DirItemInfoList>("DirItemInfoList");
    qRegisterMetaType<DirItemInfo>("DirItemInfo");
}

void DirModel::notifyItemChanged(int row)
{   
    QModelIndex first = index(row,0);
#if REGRESSION_TEST_FOLDERLISTMODEL
    QModelIndex last  = index(row, columnCount()); //Table only when testing
#else
    QModelIndex last  = first; //QML uses Listview, just one column
#endif
    emit dataChanged(first, last);
}


int DirModel::getIndex(const QString &name)
{
    QFileInfo i(name);
    return rowOfItem(DirItemInfo(i));
}


#ifndef DO_NOT_USE_TAG_LIB
QVariant DirModel::getAudioMetaData(const QFileInfo& fi, int role) const
{
    QVariant empty;
    if (!fi.isDir()) {
        TagLib::FileRef f(fi.absoluteFilePath().toStdString().c_str(), true, TagLib::AudioProperties::Fast);
        TagLib::MPEG::File mp3(fi.absoluteFilePath().toStdString().c_str(), true, TagLib::MPEG::Properties::Fast);        
        TagLib::Tag *tag = f.tag();
        if (tag)
        {
            TagLib::ID3v2::FrameList list = mp3.ID3v2Tag()->frameListMap()["APIC"];
            switch (role) {
            case TrackTitleRole:
                return QString::fromUtf8(tag->title().toCString(true));
            case TrackArtistRole:
                return QString::fromUtf8(tag->artist().toCString(true));
            case TrackAlbumRole:
                return QString::fromUtf8(tag->album().toCString(true));
            case TrackYearRole:
                return QString::number(tag->year());
            case TrackNumberRole:
                return QString::number(tag->track());
            case TrackGenreRole:
                return QString::fromUtf8(tag->genre().toCString(true));
            case TrackLengthRole:
                if(!f.isNull() && f.audioProperties()) {
                    return QString::number(f.audioProperties()->length());
                } else {
                    return QString::number(0);
                }
            case TrackCoverRole:
                if(!list.isEmpty()) {
                    TagLib::ID3v2::AttachedPictureFrame *Pic = static_cast<TagLib::ID3v2::AttachedPictureFrame *>(list.front());
                    QImage img;
                    img.loadFromData((const uchar *) Pic->picture().data(), Pic->picture().size());
                    return img;
                }
            default:
                break;
            } //switch
        }//if (tag)
    }
    return empty;
}
#endif
