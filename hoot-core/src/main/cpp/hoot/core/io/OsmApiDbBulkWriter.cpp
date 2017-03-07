/*
 * This file is part of Hootenanny.
 *
 * Hootenanny is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --------------------------------------------------------------------
 *
 * The following copyright notices are generated automatically. If you
 * have a new notice to add, please use the format:
 * " * @copyright Copyright ..."
 * This will properly maintain the copyright information. DigitalGlobe
 * copyrights will be updated automatically.
 *
 * @copyright Copyright (C) 2016, 2017 DigitalGlobe (http://www.digitalglobe.com/)
 */

#include "OsmApiDbBulkWriter.h"

#include <QDateTime>
#include <QFileInfo>

#include <hoot/core/util/HootException.h>
#include <hoot/core/util/Factory.h>
#include <hoot/core/util/Settings.h>
#include <hoot/core/visitors/ElementCountVisitor.h>
#include <hoot/core/visitors/FilteredVisitor.h>
#include <hoot/core/filters/ChainCriterion.h>
#include <hoot/core/filters/ElementTypeCriterion.h>
#include <hoot/core/util/DbUtils.h>

namespace hoot
{

HOOT_FACTORY_REGISTER(OsmMapWriter, OsmApiDbBulkWriter)

OsmApiDbBulkWriter::OsmApiDbBulkWriter()
{
  _reset();
  _sectionNames = _createSectionNameList();
  setConfiguration(conf());
}

OsmApiDbBulkWriter::~OsmApiDbBulkWriter()
{
  close();
}

bool OsmApiDbBulkWriter::isSupported(QString urlStr)
{
  QUrl url(urlStr);
  return _database.isSupported(url);
}

void OsmApiDbBulkWriter::open(QString url)
{
  if (_database.getDB().isOpen())
  {
    throw HootException(
      "Database already open.  Close the existing database connection before opening a new one.");
  }

  // Make sure we're not already open and the URL is valid
  if (!isSupported(url))
  {
    throw HootException(QString("Could not open URL ") + url);
  }

  _outputUrl = url;
  _database.open(_outputUrl);

  //offline mode doesn't assume other writers, so just grab the current IDs now and that's what
  //we'll go with, without worrying about them changing while we're writing the temp output file
  if (_mode == "offline")
  {
    _getLatestIdsFromDb();
  }
}

void OsmApiDbBulkWriter::close()
{
  _database.close();

  _reset();
  _sectionNames = _createSectionNameList();
  setConfiguration(conf());
}

void OsmApiDbBulkWriter::finalizePartial()
{
  if ((_writeStats.nodesWritten == 0) && (_writeStats.waysWritten == 0) &&
      (_writeStats.relationsWritten == 0))
  {
    LOG_DEBUG("No data written.");
    return;
  }

  shared_ptr<QTemporaryFile> sqlOutputFile(new QTemporaryFile());
  if (!sqlOutputFile->open())
  {
    throw HootException("Could not open temp file for SQL output: " + sqlOutputFile->fileName());
  }
  LOG_TRACE(sqlOutputFile->fileName());

  // Start initial section that holds nothing but UTF-8 byte-order mark (BOM)
  _createTable("byte_order_mark", "\n", true);
  // Do we have an unfinished changeset that needs flushing?
  if (_changesetData.changesInChangeset > 0)
  {
    _writeChangesetToTable();
  }
  //TODO: This may not be needed.
  if (_changesetData.changesetsWritten == 0)
  {
    _changesetData.changesetsWritten++;
  }

  if (_mode == "offline")
  {
    //In offline mode we're not guaranteeing id uniqueness, so we prepend the setval statements
    //to the element sql and don't worry if other writers wrote data while we were serializing the
    //sql file.  Here, the current IDs represent the next ID for each sequence immediately
    //after the data we're about to write (the starting IDs were obtained from the db when it
    //was opened).  Since the IDs were incremented after parsing each piece of data and represent
    //the next ID, and we want the sequence to reflect the current ID, we decrement each one.
    _writeSequenceUpdates(_changesetData.currentChangesetId - 1, _idMappings.currentNodeId - 1,
                          _idMappings.currentWayId - 1, _idMappings.currentRelationId - 1);
  }

  _writeCombinedSqlFile(sqlOutputFile);

  //if we're in online mode, we'll be writing a completely new sql output file
  shared_ptr<QTemporaryFile> finalSqlOutputFile = sqlOutputFile;
  if (_mode == "online")
  {
    _lockIds();
    finalSqlOutputFile = _updateIdOffsetsInNewFile(sqlOutputFile);
  }

  if (!_sqlFileCopyLocation.isEmpty())
  {
    QFile copyFile(_sqlFileCopyLocation);
    if (copyFile.exists())
    {
      copyFile.remove();
    }
    LOG_INFO("Copying SQL output file to " << _sqlFileCopyLocation << "...");
    if (!finalSqlOutputFile->copy(_sqlFileCopyLocation))
    {
      LOG_WARN("Unable to copy SQL output file to " << _sqlFileCopyLocation);
    }
    else
    {
      LOG_DEBUG("Copied SQL file output to " << _sqlFileCopyLocation);
    }
  }

  if (_executeSql)
  {
    _executeElementSql(finalSqlOutputFile->fileName());
  }
  else
  {
    LOG_DEBUG("Skipping SQL execution against database due to configuration...");
  }


  LOG_DEBUG("Write stats:");
  LOG_DEBUG("\tNodes written: " + QString::number(_writeStats.nodesWritten));
  LOG_DEBUG("\tNode tags written: " + QString::number(_writeStats.nodeTagsWritten));
  LOG_DEBUG("\tWays written: " + QString::number(_writeStats.waysWritten));
  LOG_DEBUG("\tWay nodes written: " + QString::number(_writeStats.wayNodesWritten));
  LOG_DEBUG("\tWay tags written: " + QString::number(_writeStats.wayTagsWritten));
  LOG_DEBUG("\tRelations written: " + QString::number(_writeStats.relationsWritten));
  LOG_DEBUG("\tRelation members written:" + QString::number(_writeStats.relationMembersWritten));
  LOG_DEBUG("\tRelation tags written: " + QString::number(_writeStats.relationTagsWritten));
  LOG_DEBUG("\tUnresolved relation members:" + QString::number(_writeStats.relationMembersWritten));
  LOG_DEBUG("\tChangesets written: " + QString::number(_changesetData.changesetsWritten));
  LOG_DEBUG("\tTotal records written: " + QString::number(_getTotalRecordsWritten()));
}

void OsmApiDbBulkWriter::_writeCombinedSqlFile(shared_ptr<QTemporaryFile> sqlTempOutputFile)
{
  try
  {
    QString totalPasses = "1";
    if (_mode == "online")
    {
      totalPasses = "2";
    }

    LOG_INFO("Writing SQL output file.  Data pass #1 of " + totalPasses + "...");
    LOG_VART(sqlTempOutputFile->fileName());

    LOG_VART(_sectionNames.size());
    LOG_VART(_outputSections.size());
    LOG_VART(_statusUpdateInterval);
    LOG_VART(_fileOutputLineBufferSize);

    QTextStream outStream(sqlTempOutputFile.get());
    outStream << "BEGIN TRANSACTION;" << "\n";
    outStream.flush();
    long totalLineCtr = 0;
    for (QStringList::const_iterator it = _sectionNames.begin(); it != _sectionNames.end(); ++it)
    {
      if (_outputSections.find(*it) == _outputSections.end())
      {
        LOG_TRACE("No data for table " + *it);
        continue;
      }

      if (_mode == "online" && *it == "sequence_updates")
      {
        //sequences are written straight to the db in online mode and are exec'd separately
        //before the element sql is exec'd
        LOG_TRACE("Skipping sequence updates in initial master file write...");
        continue;
      }

      LOG_TRACE(
        "Flushing section " << *it << " to file " << (_outputSections[*it].first)->fileName());

      // Write close marker for table
      if ((*it != "byte_order_mark") && (*it != "sequence_updates"))
      {
        LOG_TRACE("Writing byte order mark to stream...");
        *(_outputSections[*it].second) << QString("\\.\n\n\n");
      }

      // Flush any residual content from text stream/file
      (_outputSections[*it].second)->flush();
      if ((_outputSections[*it].first)->flush() == false)
      {
        throw HootException("Could not flush tempfile for table " + *it);
      }

      // Append contents of section subfiles to output file
      QFile tempInputFile(_outputSections[*it].first->fileName());
      try
      {
        if (tempInputFile.open(QIODevice::ReadOnly))
        {
          QTextStream inStream(&tempInputFile);
          QString line;
          long lineCtr = 0;
          do
          {
            line = inStream.readLine();
            LOG_VART(line);
            outStream << line << "\n";
            lineCtr++;
            totalLineCtr++;

            if (lineCtr == _fileOutputLineBufferSize)
            {
              outStream.flush();
              lineCtr = 0;
            }

            if ((totalLineCtr % _statusUpdateInterval) == 0)
            {
              QString msg =
                "Parsed " + QString::number(totalLineCtr) + " SQL lines for SQL output file.";
                msg += "  Data pass #1 of " + totalPasses + ".";
                LOG_DEBUG(msg);
            }
          }
          while (!line.isNull());
          outStream.flush();

          tempInputFile.close();
          //remove temp file after write to the output file
          LOG_TRACE("Removing temp file for " << *it << "...");
          _outputSections[*it].first->remove();
        }
        else
        {
          throw HootException("Unable to open temp input file: " + tempInputFile.fileName());
        }
      }
      catch (const Exception& e)
      {
        tempInputFile.close();
        throw e;
      }

      LOG_TRACE("Wrote contents of section " << *it);
    }
    outStream << "COMMIT;";
    outStream.flush();
    sqlTempOutputFile->close();

    QString msg = "Parsed " + QString::number(totalLineCtr) + " total SQL lines for output file.";
    msg += "  Data pass #1 of " + totalPasses + ".";
    LOG_DEBUG(msg);

    QFileInfo outputInfo(sqlTempOutputFile->fileName());
    LOG_VART(outputInfo.size());
    LOG_DEBUG("SQL file write complete.  Data pass #1 of " + totalPasses + "...");
  }
  catch (const Exception& e)
  {
    if (sqlTempOutputFile.get())
    {
      sqlTempOutputFile->close();
    }
    throw e;
  }
}

shared_ptr<QTemporaryFile> OsmApiDbBulkWriter::_updateIdOffsetsInNewFile(
  shared_ptr<QTemporaryFile> inputSqlFile)
{
  shared_ptr<QTemporaryFile> updateSqlOutputFile(new QTemporaryFile());
  try
  {
    LOG_INFO("Updating ID offsets in SQL file.  Data pass #2 of 2...");
    LOG_VART(inputSqlFile->fileName());
     QFileInfo inputInfo(inputSqlFile->fileName());
     LOG_VART(inputInfo.size());

     if (!updateSqlOutputFile->open())
     {
       throw HootException("Could not open file for SQL output: " + updateSqlOutputFile->fileName());
     }

     QTextStream outStream(updateSqlOutputFile.get());
     if (!inputSqlFile->open())
     {
       throw HootException(
         "Error opening input SQL file for ID offset updates: " + inputSqlFile->fileName());
     }
     QTextStream inStream(inputSqlFile.get());

     QString line;
     long lineCtr = 0;
     long totalLineCtr = 0;
     QString currentTableName = "";
     do
     {
       line = inStream.readLine().trimmed();
       LOG_VART(line);

       if (line.contains("COPY"))
       {
         LOG_VART(line.split(QRegExp("\\s")).size());
         currentTableName = line.split(QRegExp("\\s"))[1].trimmed();
         LOG_VART(currentTableName);
       }
       else if (!line.isEmpty() && line != "\\.")
       {
         LOG_TRACE("Checking for ID update...");
         LOG_VART(currentTableName);

         QStringList lineParts = line.split("\t");

         //update any element/changeset ID reference by adding the current ID offset to it

         bool lineUpdated = true;
         if (currentTableName == ApiDb::getChangesetsTableName())
         {
           lineParts[0] = QString::number(lineParts[0].toLong() + _changesetData.currentChangesetId);
         }
         else if (currentTableName == ApiDb::getCurrentNodesTableName() ||
                  currentTableName == ApiDb::getNodesTableName())
         {
           lineParts[0] = QString::number(lineParts[0].toLong() + _idMappings.currentNodeId);
           lineParts[3] = QString::number(lineParts[3].toLong() + _changesetData.currentChangesetId);
         }
         else if (currentTableName == ApiDb::getCurrentWaysTableName() ||
                  currentTableName == ApiDb::getWaysTableName())
         {
           lineParts[0] = QString::number(lineParts[0].toLong() + _idMappings.currentWayId);
           lineParts[1] = QString::number(lineParts[1].toLong() + _changesetData.currentChangesetId);
         }
         else if (currentTableName == ApiDb::getCurrentWayNodesTableName() ||
                  currentTableName == ApiDb::getWayNodesTableName())
         {
           lineParts[0] = QString::number(lineParts[0].toLong() + _idMappings.currentWayId);
           lineParts[1] = QString::number(lineParts[1].toLong() + _idMappings.currentNodeId);
         }
         else if (currentTableName == ApiDb::getCurrentRelationsTableName() ||
                  currentTableName == ApiDb::getRelationsTableName())
         {
           lineParts[0] = QString::number(lineParts[0].toLong() + _idMappings.currentRelationId);
           lineParts[1] = QString::number(lineParts[1].toLong() + _changesetData.currentChangesetId);
         }
         else if (currentTableName == ApiDb::getCurrentRelationMembersTableName() ||
                  currentTableName == ApiDb::getRelationMembersTableName())
         {
           lineParts[0] = QString::number(lineParts[0].toLong() + _idMappings.currentRelationId);
           const long memberId = lineParts[2].toLong();
           if (lineParts[1].toLower() == "node")
           {
             lineParts[2] = QString::number(memberId + _idMappings.currentNodeId);
           }
           else if (lineParts[1].toLower() == "way")
           {
             lineParts[2] = QString::number(memberId + _idMappings.currentWayId);
             }
         }
         else if (currentTableName == ApiDb::getCurrentNodeTagsTableName() ||
                  currentTableName == ApiDb::getNodeTagsTableName())
         {
           lineParts[0] = QString::number(lineParts[0].toLong() + _idMappings.currentNodeId);
         }
         else if (currentTableName == ApiDb::getCurrentWayTagsTableName() ||
                  currentTableName == ApiDb::getWayTagsTableName())
         {
           lineParts[0] = QString::number(lineParts[0].toLong() + _idMappings.currentWayId);
         }
         else if (currentTableName == ApiDb::getCurrentRelationTagsTableName() ||
                  currentTableName == ApiDb::getRelationTagsTableName())
         {
           lineParts[0] = QString::number(lineParts[0].toLong() + _idMappings.currentRelationId);
         }
         else
         {
           lineUpdated = false;
         }

         if (lineUpdated)
         {
           line = lineParts.join("\t");
           LOG_TRACE("Updated line: " << line);
         }
       }
       else
       {
         //next line is not a record
         LOG_TRACE("Passing line through to output with no changes...");
         currentTableName = "";
       }

       if (!line.isEmpty())
       {
         outStream << line;
       }
       outStream << "\n";
       lineCtr++;
       totalLineCtr++;

       if (lineCtr == _fileOutputLineBufferSize)
       {
         outStream.flush();
         lineCtr = 0;
       }

       if (totalLineCtr % _statusUpdateInterval == 0)
       {
         LOG_DEBUG(
           "Parsed " << totalLineCtr << " lines for SQL ID offset updates in SQL output " <<
           "file.  Data pass #2 of 2.");
       }
     }
     while (!line.isNull());
     outStream.flush();

     inputSqlFile->close();
     updateSqlOutputFile->close();

     QFileInfo outputInfo(updateSqlOutputFile->fileName());
     LOG_VARD(outputInfo.size());

     LOG_DEBUG(
       "Parsed " << totalLineCtr << " total lines for SQL ID offset updates in SQL output " <<
       "file.  Data pass #2 of 2.");
     LOG_DEBUG("ID offset updates complete.  Data pass #2 of 2.");
  }
  catch (const Exception& e)
  {
    if (inputSqlFile.get())
    {
      inputSqlFile->close();
    }
    if (updateSqlOutputFile.get())
    {
      updateSqlOutputFile->close();
    }
    throw e;
  }

  return updateSqlOutputFile;
}

void OsmApiDbBulkWriter::_lockIds()
{
  //this assumes the data has already been written out to sql file once and _writeStats has valid
  //values for the number of elements written
  if (_writeStats.nodesWritten == 0)
  {
    throw HootException(
      QString("OSM API database bulk writer cannot lock out element ID range if no element SQL ") +
      QString("has been written to the temporary file."));
  }

  _getLatestIdsFromDb();

  //We need to prevent other writers from claiming the IDs associated with the elements we're
  //about to write.  Before the potentially lengthy SQL file ID update process which will happen
  //next, lock out the ID range starting with the next ID in each sequence we just obtained from
  //the db up to the number of each sequence type we're writing.

  //write the id lock sql out to a temp file
  _writeSequenceUpdates(_changesetData.currentChangesetId + _changesetData.changesetsWritten,
                        _idMappings.currentNodeId + _writeStats.nodesWritten,
                        _idMappings.currentWayId + _writeStats.waysWritten,
                        _idMappings.currentRelationId + _writeStats.relationsWritten);
  (_outputSections["sequence_updates"].second)->flush();
  if (!(_outputSections["sequence_updates"].first)->flush())
  {
    throw HootException("Could not flush tempfile for table sequence_updates.");
  }

  //read in and exec the id lock sql for the needed ID ranges out for this element write; We're
  //not worrying about cleaning up the locked out ID ranges if this db write ends up failing.
  QString lockElementIdsSql;
  QFile sequenceFile(_outputSections["sequence_updates"].first->fileName());
  if (sequenceFile.open(QIODevice::ReadOnly))
  {
    QTextStream sequenceStrm(&sequenceFile);
    lockElementIdsSql = sequenceStrm.readAll();
  }
  else
  {
    throw HootException("Unable to open sequence updates file.");
  }
  sequenceFile.close();

  LOG_INFO("Writing sequence ID updates to database...");
  _database.transaction();
  DbUtils::execNoPrepare(_database.getDB(), lockElementIdsSql);
  _database.commit();
  LOG_DEBUG("Sequence updates written to database.");
}

void OsmApiDbBulkWriter::_executeElementSql(const QString sqlFile)
{
  //exec element sql against the db; Using psql here b/c I'm assuming it is doing buffered reads
  //against the sql file, so no need to handle buffering the sql read manually and applying it to a
  //QSqlQuery.
  LOG_INFO("Executing element SQL for " << _getTotalRecordsWritten() << " records...");
  QMap<QString, QString> dbUrlParts = ApiDb::getDbUrlParts(_outputUrl);
  QString cmd = "export PGPASSWORD=" + dbUrlParts["password"] + "; psql";
  if (!(Log::getInstance().getLevel() <= Log::Debug))
  {
    cmd += " --quiet";
  }
  cmd += " " + ApiDb::getPsqlString(_outputUrl) + " -f " + sqlFile;
  if (!(Log::getInstance().getLevel() <= Log::Debug))
  {
    cmd += " > /dev/null";
  }
  LOG_DEBUG(cmd);
  if (system(cmd.toStdString().c_str()) != 0)
  {
    throw HootException("Failed executing bulk element SQL write against the OSM API database.");
  }
  LOG_INFO("Element SQL execution complete.");
}

long OsmApiDbBulkWriter::_getTotalRecordsWritten() const
{
  return
    _writeStats.nodesWritten + _writeStats.nodeTagsWritten + _writeStats.relationMembersWritten +
    _writeStats.relationsWritten + _writeStats.relationTagsWritten + _writeStats.wayNodesWritten +
    _writeStats.waysWritten + _writeStats.wayTagsWritten + _changesetData.changesetsWritten;
}

void OsmApiDbBulkWriter::_getLatestIdsFromDb()
{
  //get the current ID sequence for each element type from the database
  LOG_DEBUG("Retrieving current IDs from database...");

  _idMappings.currentNodeId = _database.getNextId(ElementType::Node);
  _idMappings.currentWayId = _database.getNextId(ElementType::Way);
  _idMappings.currentRelationId = _database.getNextId(ElementType::Relation);
  _changesetData.currentChangesetId = _database.getNextId(ApiDb::getChangesetsTableName());
  //TODO: why does this need to be done?
  if (_mode == "online")
  {
    _idMappings.currentNodeId--;
    _idMappings.currentWayId--;
    _idMappings.currentRelationId--;
    _changesetData.currentChangesetId--;
  }

  LOG_VARD(_changesetData.currentChangesetId);
  LOG_VARD(_idMappings.currentNodeId);
  LOG_VARD(_idMappings.currentWayId);
  LOG_VARD(_idMappings.currentRelationId);
}

void OsmApiDbBulkWriter::writePartial(const ConstNodePtr& n)
{
  LOG_VART(n);

  //Since we're only creating elements, the changeset bounds is simply the combined bounds
  //of all the nodes involved in the changeset.
  //TODO: This actually won't work when ways or relations are written in separate changesets than
  //the nodes they reference.  Since we're streaming the elements, there's no way to get back to
  //the bounds information.  This bug has always been here, but just recently noticed.
  _changesetData.changesetBounds.expandToInclude(n->getX(), n->getY());
  LOG_VART(_changesetData.changesetBounds.toString());

  if (_writeStats.nodesWritten == 0)
  {
    _createNodeTables();
    _idMappings.nodeIdMap = shared_ptr<BigMap<long, long> >(new BigMap<long, long>());
  }

  long nodeDbId;
  // Do we already know about this node?
  if (_idMappings.nodeIdMap->contains(n->getId()) == true)
  {
    throw NotImplementedException("Writer class does not support update operations.");
  }
  // Have to establish new mapping
  nodeDbId = _establishNewIdMapping(n->getElementId());
  LOG_VART(nodeDbId);

  _writeNodeToTables(n, nodeDbId);
  _writeTagsToTables(n->getTags(), nodeDbId,
    _outputSections[ApiDb::getCurrentNodeTagsTableName()].second, "%1\t%2\t%3\n",
    _outputSections[ApiDb::getNodeTagsTableName()].second, "%1\t1\t%2\t%3\n");
  _writeStats.nodesWritten++;
  _writeStats.nodeTagsWritten += n->getTags().size();

  _incrementChangesInChangeset();

  _checkUnresolvedReferences(n, nodeDbId);

  if (_writeStats.nodesWritten % _statusUpdateInterval == 0)
  {
    LOG_DEBUG("Parsed " << _writeStats.nodesWritten << " nodes.");
  }
}

void OsmApiDbBulkWriter::writePartial(const ConstWayPtr& w)
{
  LOG_VART(w);

  if (_writeStats.waysWritten == 0)
  {
    _createWayTables();
    _idMappings.wayIdMap = shared_ptr<BigMap<long, long> >(new BigMap<long, long>());
  }

  long wayDbId;
  // Do we already know about this way?
  if (_idMappings.wayIdMap->contains(w->getId()) == true)
  {
    throw hoot::NotImplementedException("Writer class does not support update operations");
  }
  // Have to establish new mapping
  wayDbId = _establishNewIdMapping(w->getElementId());
  LOG_VART(wayDbId);

  _writeWayToTables(wayDbId);
  _writeWaynodesToTables(_idMappings.wayIdMap->at(w->getId()), w->getNodeIds());
  _writeTagsToTables(w->getTags(), wayDbId,
    _outputSections[ApiDb::getCurrentWayTagsTableName()].second, "%1\t%2\t%3\n",
    _outputSections[ApiDb::getWayTagsTableName()].second, "%1\t1\t%2\t%3\n");
  _writeStats.waysWritten++;
  _writeStats.wayTagsWritten += w->getTags().size();
  _writeStats.wayNodesWritten += w->getNodeIds().size();

  _incrementChangesInChangeset();

  _checkUnresolvedReferences(w, wayDbId);

  if (_writeStats.waysWritten % _statusUpdateInterval == 0)
  {
    LOG_DEBUG("Parsed " << _writeStats.waysWritten << " ways.");
  }
}

void OsmApiDbBulkWriter::writePartial(const ConstRelationPtr& r)
{
  LOG_VART(r);

  if (_writeStats.relationsWritten == 0)
  {
    _createRelationTables();
    _idMappings.relationIdMap = shared_ptr<BigMap<long, long> >(new BigMap<long, long>());
  }

  long relationDbId;
  // Do we already know about this node?
  if (_idMappings.relationIdMap->contains(r->getId()) == true)
  {
    throw hoot::NotImplementedException("Writer class does not support update operations");
  }
  // Have to establish new mapping
  relationDbId = _establishNewIdMapping(r->getElementId());
  LOG_VART(relationDbId);

  _writeRelationToTables(relationDbId);
  _writeRelationMembersToTables(r);
  _writeTagsToTables(r->getTags(), relationDbId,
    _outputSections[ApiDb::getCurrentRelationTagsTableName()].second, "%1\t%2\t%3\n",
    _outputSections[ApiDb::getRelationTagsTableName()].second, "%1\t1\t%2\t%3\n");
  _writeStats.relationsWritten++;
  _writeStats.relationTagsWritten += r->getTags().size();
  _writeStats.relationMembersWritten += r->getMembers().size();

  _incrementChangesInChangeset();

  _checkUnresolvedReferences(r, relationDbId);

  if (_writeStats.relationsWritten % _statusUpdateInterval == 0)
  {
    LOG_DEBUG("Parsed " << _writeStats.relationsWritten << " relations.");
  }
}

void OsmApiDbBulkWriter::setConfiguration(const hoot::Settings& conf)
{
  const ConfigOptions confOptions(conf);
  _changesetData.changesetUserId = confOptions.getChangesetUserId();
  setMode(confOptions.getOsmapidbBulkWriterMode().toLower());
  LOG_DEBUG("OSM API database bulk writer set to " << _mode << " mode.");
  setFileOutputLineBufferSize(confOptions.getOsmapidbBulkWriterFileOutputBufferMaxLineSize());
  setStatusUpdateInterval(confOptions.getOsmapidbBulkWriterFileOutputStatusUpdateInterval());
  setSqlFileCopyLocation(confOptions.getOsmapidbBulkWriterSqlOutputFileCopyLocation().trimmed());
  setExecuteSql(confOptions.getOsmapidbBulkWriterExecuteSql());
}

QStringList OsmApiDbBulkWriter::_createSectionNameList()
{
  QStringList sections;

  sections.push_back(QString("byte_order_mark"));
  sections.push_back(QString("sequence_updates"));
  sections.push_back(ApiDb::getChangesetsTableName());
  sections.push_back(ApiDb::getCurrentNodesTableName());
  sections.push_back(ApiDb::getCurrentNodeTagsTableName());
  sections.push_back(ApiDb::getNodesTableName());
  sections.push_back(ApiDb::getNodeTagsTableName());
  sections.push_back(ApiDb::getCurrentWaysTableName());
  sections.push_back(ApiDb::getCurrentWayNodesTableName());
  sections.push_back(ApiDb::getCurrentWayTagsTableName());
  sections.push_back(ApiDb::getWaysTableName());
  sections.push_back(ApiDb::getWayNodesTableName());
  sections.push_back(ApiDb::getWayTagsTableName());
  sections.push_back(ApiDb::getCurrentRelationsTableName());
  sections.push_back(ApiDb::getCurrentRelationMembersTableName());
  sections.push_back(ApiDb::getCurrentRelationTagsTableName());
  sections.push_back(ApiDb::getRelationsTableName());
  sections.push_back(ApiDb::getRelationMembersTableName());
  sections.push_back(ApiDb::getRelationTagsTableName());

  return sections;
}

void OsmApiDbBulkWriter::_createNodeTables()
{
  _createTable(ApiDb::getCurrentNodesTableName(),
                "COPY " + ApiDb::getCurrentNodesTableName() +
               " (id, latitude, longitude, changeset_id, visible, \"timestamp\", tile, version) " +
               "FROM stdin;\n" );
  _createTable(ApiDb::getCurrentNodeTagsTableName(),
                "COPY " + ApiDb::getCurrentNodeTagsTableName() +
               " (node_id, k, v) FROM stdin;\n");

  _createTable(ApiDb::getNodesTableName(),
                "COPY " + ApiDb::getNodesTableName() +
               " (node_id, latitude, longitude, changeset_id, visible, \"timestamp\", tile, version, redaction_id) FROM stdin;\n" );
  _createTable(ApiDb::getNodeTagsTableName(),
                "COPY " + ApiDb::getNodeTagsTableName() +
               " (node_id, version, k, v) FROM stdin;\n");
}

void OsmApiDbBulkWriter::_reset()
{
  LOG_DEBUG("Resetting variables...");

  _writeStats.nodesWritten = 0;
  _writeStats.nodeTagsWritten = 0;
  _writeStats.waysWritten = 0;
  _writeStats.wayNodesWritten = 0;
  _writeStats.wayTagsWritten = 0;
  _writeStats.relationsWritten = 0;
  _writeStats.relationMembersWritten = 0;
  _writeStats.relationMembersUnresolved = 0;
  _writeStats.relationTagsWritten = 0;

  _changesetData.changesetUserId = -1;
  _changesetData.currentChangesetId = 1;
  _changesetData.changesInChangeset = 0;
  _changesetData.changesetsWritten = 0;

  _idMappings.currentNodeId = 1;
  _idMappings.nodeIdMap.reset();

  _idMappings.currentWayId = 1;
  _idMappings.wayIdMap.reset();

  _idMappings.currentRelationId = 1;
  _idMappings.relationIdMap.reset();

  _unresolvedRefs.unresolvedWaynodeRefs.reset();
  _unresolvedRefs.unresolvedRelationRefs.reset();

  _outputSections.clear();
  _sectionNames.erase(_sectionNames.begin(), _sectionNames.end());
}

long OsmApiDbBulkWriter::_establishNewIdMapping(const ElementId& sourceId)
{
  long dbIdentifier;

  switch (sourceId.getType().getEnum())
  {
  case ElementType::Node:
    dbIdentifier = _idMappings.currentNodeId;
    _idMappings.nodeIdMap->insert(sourceId.getId(), dbIdentifier);
    _idMappings.currentNodeId++;
    break;

  case ElementType::Way:
    dbIdentifier = _idMappings.currentWayId;
    _idMappings.wayIdMap->insert(sourceId.getId(), dbIdentifier);
    _idMappings.currentWayId++;
    break;

  case ElementType::Relation:
    dbIdentifier = _idMappings.currentRelationId;
    _idMappings.relationIdMap->insert(sourceId.getId(), dbIdentifier);
    _idMappings.currentRelationId++;
    break;

  default:
    throw NotImplementedException("Unsupported element type");
    break;
  }

  return dbIdentifier;
}

unsigned int OsmApiDbBulkWriter::_convertDegreesToNanodegrees(const double degrees) const
{
  return (round(degrees * ApiDb::COORDINATE_SCALE));
}

void OsmApiDbBulkWriter::_writeNodeToTables(const ConstNodePtr& node, const long nodeDbId)
{
  const double nodeY = node->getY();
  const double nodeX = node->getX();
  const int nodeYNanodegrees = _convertDegreesToNanodegrees(nodeY);
  const int nodeXNanodegrees = _convertDegreesToNanodegrees(nodeX);
  const int changesetId = _changesetData.currentChangesetId;
  const QString datestring =
    QDateTime::currentDateTime().toUTC().toString("yyyy-MM-dd hh:mm:ss.zzz");
  const QString tileNumberString(QString::number(ApiDb::tileForPoint(nodeY, nodeX)));

  if ((nodeYNanodegrees < -900000000) || (nodeYNanodegrees > 900000000))
  {
    throw HootException(
      QString("Invalid latitude conversion, Y = %1 to %2").arg(
        QString::number(nodeY), QString::number(nodeYNanodegrees)));
  }
  if ((nodeXNanodegrees < -1800000000) || (nodeXNanodegrees > 1800000000))
  {
    throw HootException(
      QString("Invalid longitude conversion, X = %1 to %2").arg(
        QString::number(nodeX), QString::number(nodeXNanodegrees)));
  }

  QString outputLine =
    QString("%1\t%2\t%3\t%4\tt\t%5\t%6\t1\n").arg(
      QString::number(nodeDbId),
      QString::number(nodeYNanodegrees),
      QString::number(nodeXNanodegrees),
      QString::number(changesetId),
      datestring,
      tileNumberString);

  *(_outputSections[ApiDb::getCurrentNodesTableName()].second) << outputLine;

  outputLine =
    QString("%1\t%2\t%3\t%4\tt\t%5\t%6\t1\t\\N\n").arg(
      QString::number(nodeDbId),
      QString::number(nodeYNanodegrees),
      QString::number(nodeXNanodegrees),
      QString::number(changesetId),
      datestring,
      tileNumberString);

  *(_outputSections[ApiDb::getNodesTableName()].second) << outputLine;
}

void OsmApiDbBulkWriter::_writeTagsToTables(const Tags& tags, const long nodeDbId,
                                            shared_ptr<QTextStream>& currentTable,
                                            const QString currentTableFormatString,
                                            shared_ptr<QTextStream>& historicalTable,
                                            const QString historicalTableFormatString)
{
  const QString nodeDbIdString(QString::number(nodeDbId));

  for (Tags::const_iterator it = tags.begin(); it != tags.end(); ++it)
  {
    const QString key = _escapeCopyToData(it.key());
    LOG_VART(key);
    const QString value = _escapeCopyToData(it.value());
    LOG_VART(value);

    *currentTable << currentTableFormatString.arg(nodeDbIdString, key, value);
    *historicalTable << historicalTableFormatString.arg(nodeDbIdString, key, value);
  }
}

void OsmApiDbBulkWriter::_createWayTables()
{
  _createTable(
    ApiDb::getCurrentWaysTableName(),
    "COPY " + ApiDb::getCurrentWaysTableName() +
    " (id, changeset_id, \"timestamp\", visible, version) FROM stdin;\n");
  _createTable(
    ApiDb::getCurrentWayTagsTableName(),
    "COPY " + ApiDb::getCurrentWayTagsTableName() + " (way_id, k, v) FROM stdin;\n");
  _createTable(
    ApiDb::getCurrentWayNodesTableName(),
    "COPY " + ApiDb::getCurrentWayNodesTableName() +
    " (way_id, node_id, sequence_id) FROM stdin;\n" );

  _createTable(
    ApiDb::getWaysTableName(),
    "COPY " + ApiDb::getWaysTableName() +
    " (way_id, changeset_id, \"timestamp\", version, visible, redaction_id) FROM stdin;\n");
  _createTable(
    ApiDb::getWayTagsTableName(),
    "COPY " + ApiDb::getWayTagsTableName() +
    " (way_id, version, k, v) FROM stdin;\n");
  _createTable(
    ApiDb::getWayNodesTableName(),
    "COPY " + ApiDb::getWayNodesTableName() +
    " (way_id, node_id, version, sequence_id) FROM stdin;\n");
}

void OsmApiDbBulkWriter::_writeWayToTables(const long wayDbId)
{
  const int changesetId = _changesetData.currentChangesetId;
  const QString datestring =
    QDateTime::currentDateTime().toUTC().toString("yyyy-MM-dd hh:mm:ss.zzz");

  QString outputLine =
    QString("%1\t%2\t%3\tt\t1\n")
      .arg(wayDbId)
      .arg(changesetId)
      .arg(datestring);

  *(_outputSections[ApiDb::getCurrentWaysTableName()].second) << outputLine;

  outputLine =
    QString("%1\t%2\t%3\t1\tt\t\\N\n")
      .arg(wayDbId)
      .arg(changesetId)
      .arg(datestring);

  *(_outputSections[ApiDb::getWaysTableName()].second) << outputLine;
}

void OsmApiDbBulkWriter::_writeWaynodesToTables(const long dbWayId, const vector<long>& waynodeIds)
{
  unsigned int nodeIndex = 1;

  shared_ptr<QTextStream> currentWayNodesStream =
    _outputSections[ApiDb::getCurrentWayNodesTableName()].second;
  shared_ptr<QTextStream> wayNodesStream = _outputSections[ApiDb::getWayNodesTableName()].second;
  const QString currentWaynodesFormat("%1\t%2\t%3\n");
  const QString waynodesFormat("%1\t%2\t1\t%3\n");
  const QString dbWayIdString( QString::number(dbWayId));

  for (vector<long>::const_iterator it = waynodeIds.begin(); it != waynodeIds.end(); ++it)
  {
    if (_idMappings.nodeIdMap->contains(*it) == true)
    {
      const QString dbNodeIdString = QString::number(_idMappings.nodeIdMap->at(*it));
      const QString nodeIndexString(QString::number(nodeIndex));
      *currentWayNodesStream <<
        currentWaynodesFormat.arg(dbWayIdString, dbNodeIdString, nodeIndexString);
      *wayNodesStream << waynodesFormat.arg(dbWayIdString, dbNodeIdString, nodeIndexString);
    }
    else
    {
      throw NotImplementedException(
        "Unresolved waynodes are not supported.  " +
        QString("Way %1 has reference to unknown node ID %2").arg(dbWayId, *it));
    }

    ++nodeIndex;
  }
}

void OsmApiDbBulkWriter::_createRelationTables()
{
  _createTable(
    ApiDb::getCurrentRelationsTableName(),
    "COPY " + ApiDb::getCurrentRelationsTableName() +
    " (id, changeset_id, \"timestamp\", visible, version) FROM stdin;\n");
  _createTable(
    ApiDb::getCurrentRelationTagsTableName(),
    "COPY " + ApiDb::getCurrentRelationTagsTableName() + " (relation_id, k, v) FROM stdin;\n");
  _createTable(
    ApiDb::getCurrentRelationMembersTableName(),
    "COPY " + ApiDb::getCurrentRelationMembersTableName() +
    " (relation_id, member_type, member_id, member_role, sequence_id) FROM stdin;\n");

  _createTable(
    ApiDb::getRelationsTableName(),
    "COPY " + ApiDb::getRelationsTableName() +
    " (relation_id, changeset_id, \"timestamp\", version, visible, redaction_id) FROM stdin;\n");
  _createTable(
    ApiDb::getRelationTagsTableName(),
    "COPY " + ApiDb::getRelationTagsTableName() +
    " (relation_id, version, k, v) FROM stdin;\n" );
  _createTable(
    ApiDb::getRelationMembersTableName(),
    "COPY " + ApiDb::getRelationMembersTableName() +
    " (relation_id, member_type, member_id, member_role, version, sequence_id) FROM stdin;\n");
}

void OsmApiDbBulkWriter::_writeRelationToTables(const long relationDbId)
{
  const int changesetId = _changesetData.currentChangesetId;
  const QString datestring =
  QDateTime::currentDateTime().toUTC().toString("yyyy-MM-dd hh:mm:ss.zzz");

  QString outputLine =
    QString("%1\t%2\t%3\tt\t1\n")
      .arg(relationDbId)
      .arg(changesetId)
      .arg(datestring);

  *(_outputSections[ApiDb::getCurrentRelationsTableName()].second) << outputLine;

  outputLine =
    QString("%1\t%2\t%3\t1\tt\t\\N\n")
      .arg(relationDbId)
      .arg(changesetId)
      .arg(datestring);

  *(_outputSections[ApiDb::getRelationsTableName()].second) << outputLine;
}

void OsmApiDbBulkWriter::_writeRelationMembersToTables(const ConstRelationPtr& relation)
{
  unsigned int memberSequenceIndex = 1;
  const long relationId = relation->getId();
  const long dbRelationId = _idMappings.relationIdMap->at(relationId);
  const vector<RelationData::Entry> relationMembers = relation->getMembers();
  shared_ptr<BigMap<long, long> > knownElementMap;

  for (vector<RelationData::Entry>::const_iterator it = relationMembers.begin();
       it != relationMembers.end(); ++it)
  {
    const ElementId memberElementId = it->getElementId();

    switch (memberElementId.getType().getEnum())
    {
    case ElementType::Node:
      knownElementMap = _idMappings.nodeIdMap;
      break;

    case ElementType::Way:
      knownElementMap = _idMappings.wayIdMap;
      break;

    case ElementType::Relation:
      knownElementMap = _idMappings.relationIdMap;
      break;

    default:
      throw HootException("Unsupported element member type");
      break;
    }

    if ((knownElementMap != shared_ptr<BigMap<long, long> >())
          && (knownElementMap->contains(memberElementId.getId()) == true))
    {
      _writeRelationMember(
        dbRelationId, *it, knownElementMap->at(memberElementId.getId()), memberSequenceIndex);
    }
    else
    {
      if (_unresolvedRefs.unresolvedRelationRefs ==
          shared_ptr<map<ElementId, UnresolvedRelationReference > >())
      {
        _unresolvedRefs.unresolvedRelationRefs =
          shared_ptr<map<ElementId, UnresolvedRelationReference > >(
            new map<ElementId, UnresolvedRelationReference>());
      }

      const UnresolvedRelationReference relationRef =
        { relationId, dbRelationId, *it, memberSequenceIndex };

      _unresolvedRefs.unresolvedRelationRefs->insert(
        pair<ElementId, UnresolvedRelationReference>(memberElementId, relationRef));
    }

    ++memberSequenceIndex;
  }
}

void OsmApiDbBulkWriter::_writeRelationMember(const long sourceRelationDbId,
                                              const RelationData::Entry& memberEntry,
                                              const long memberDbId,
                                              const unsigned int memberSequenceIndex)
{
  QString memberType;
  const ElementId memberElementId = memberEntry.getElementId();

  switch (memberElementId.getType().getEnum())
  {
  case ElementType::Node:
    memberType = "Node";
    break;

  case ElementType::Way:
    memberType = "Way";
    break;

  case ElementType::Relation:
    memberType = "Relation";
    break;

  default:
    throw HootException("Unsupported element member type");
    break;
  }

  const QString dbRelationIdString(QString::number(sourceRelationDbId));
  const QString memberRefIdString(QString::number(memberDbId));
  const QString memberSequenceString(QString::number(memberSequenceIndex));
  const QString memberRole = _escapeCopyToData(memberEntry.getRole());
  shared_ptr<QTextStream> currentRelationMembersStream =
    _outputSections[ApiDb::getCurrentRelationMembersTableName()].second;
  shared_ptr<QTextStream> relationMembersStream =
    _outputSections[ApiDb::getRelationMembersTableName()].second;
  const QString currentRelationMemberFormat("%1\t%2\t%3\t%4\t%5\n");
  const QString relationMembersFormat("%1\t%2\t%3\t%4\t1\t%5\n");

  *currentRelationMembersStream << currentRelationMemberFormat.arg(
    dbRelationIdString, memberType, memberRefIdString, memberRole, memberSequenceString);
  *relationMembersStream << relationMembersFormat.arg(
    dbRelationIdString, memberType, memberRefIdString, memberRole, memberSequenceString);

  _writeStats.relationMembersWritten++;
}

void OsmApiDbBulkWriter::_createTable(const QString tableName, const QString tableHeader)
{
  _createTable(tableName, tableHeader, false);
}

void OsmApiDbBulkWriter::_createTable(const QString tableName, const QString tableHeader,
                                      const bool addByteOrderMark)
{
  shared_ptr<QTemporaryFile> tempfile(new QTemporaryFile());
  if (tempfile->open() == false)
  {
    throw HootException(
      "Could not open temp file at: " + tempfile->fileName() + " for contents of table " +
      tableName);
  }
  tempfile->setAutoRemove(false);

  _outputSections[tableName] =
    pair<shared_ptr<QTemporaryFile>, shared_ptr<QTextStream> >(
      tempfile, shared_ptr<QTextStream>(new QTextStream(tempfile.get())));

  // Database is encoded in UTF-8, so force encoding as otherwise file is in local
  //    Western encoding which goes poorly for a lot of countries
  _outputSections[tableName].second->setCodec("UTF-8");

  // First table written out should have byte order mark to help identifify content as UTF-8
  if (addByteOrderMark == true)
  {
    _outputSections[tableName].second->setGenerateByteOrderMark(true);
  }

  *(_outputSections[tableName].second) << tableHeader;
}

void OsmApiDbBulkWriter::_incrementChangesInChangeset()
{
  _changesetData.changesInChangeset++;
  if (_changesetData.changesInChangeset == _maxChangesetSize)
  {
    LOG_VART(_changesetData.changesInChangeset);
    _writeChangesetToTable();
    long changesetUpdateInterval;
    if (_statusUpdateInterval > _maxChangesetSize)
    {
      changesetUpdateInterval = _statusUpdateInterval / _maxChangesetSize;
    }
    else
    {
      changesetUpdateInterval = _statusUpdateInterval;
    }
    if (_changesetData.changesetsWritten % changesetUpdateInterval == 0)
    {
      LOG_DEBUG("Parsed " << _changesetData.changesetsWritten << " changesets.");
    }
    _changesetData.currentChangesetId++;
    LOG_VART(_changesetData.currentChangesetId);
    _changesetData.changesInChangeset = 0;
    _changesetData.changesetBounds.init();
    _changesetData.changesetsWritten++;
    LOG_VART(_changesetData.changesetsWritten);
  }
}

void OsmApiDbBulkWriter::_checkUnresolvedReferences(const ConstElementPtr& element,
                                                    const long elementDbId)
{
  // Regardless of type, may be referenced in relation
  if (_unresolvedRefs.unresolvedRelationRefs !=
      shared_ptr<map<ElementId, UnresolvedRelationReference > >())
  {
    map<ElementId, UnresolvedRelationReference >::iterator relationRef =
      _unresolvedRefs.unresolvedRelationRefs->find(element->getElementId());

    if (relationRef != _unresolvedRefs.unresolvedRelationRefs->end())
    {
      LOG_DEBUG("Found unresolved relation member ref!");
      LOG_DEBUG(QString( "Relation ID ") + QString::number(relationRef->second.sourceRelationId) +
        QString(" (DB ID=") + QString::number(relationRef->second.sourceDbRelationId) +
        QString(") has ref to ") + relationRef->second.relationMemberData.toString());

      _writeRelationMember(
        relationRef->second.sourceDbRelationId, relationRef->second.relationMemberData,
        elementDbId, relationRef->second.relationMemberSequenceId);

      // Remove entry from unresolved list
      _unresolvedRefs.unresolvedRelationRefs->erase(relationRef);
    }
  }

  // If newly-written element is a node, check noderefs as well
  if (element->getElementType().getEnum() == ElementType::Node)
  {
    if ((_unresolvedRefs.unresolvedWaynodeRefs !=
        shared_ptr<BigMap<long, vector<pair<long, unsigned long> > > >()) &&
        (_unresolvedRefs.unresolvedWaynodeRefs->contains(element->getId()) == true))
    {
      throw NotImplementedException(
        "Found unresolved waynode ref!  For node: " + QString::number(element->getId()) +
        " Need to insert waynode ref that is now resolved");
    }
  }
}

QString OsmApiDbBulkWriter::_escapeCopyToData(const QString stringToOutput) const
{
  QString escapedString(stringToOutput);

  // Escape any special characters as required by
  //    http://www.postgresql.org/docs/9.2/static/sql-copy.html
  escapedString.replace(QChar(92), QString("\\\\"));  // Escape single backslashes first
  escapedString.replace(QChar(8), QString("\\b"));
  escapedString.replace(QChar(9), QString("\\t"));
  escapedString.replace(QChar(10), QString("\\n"));
  escapedString.replace(QChar(11), QString("\\v"));
  escapedString.replace(QChar(12), QString("\\f"));
  escapedString.replace(QChar(13), QString("\\r"));

  return escapedString;
}

void OsmApiDbBulkWriter::_writeChangesetToTable()
{
  if (_changesetData.changesetUserId == -1)
  {
    throw HootException(
      "Invalid changeset user ID: " + QString::number(_changesetData.changesetUserId));
  }
  LOG_VART(_changesetData.changesetUserId);

  if (_changesetData.currentChangesetId == 1)
  {
    _createTable(
      ApiDb::getChangesetsTableName(),
      "COPY " + ApiDb::getChangesetsTableName() +
      " (id, user_id, created_at, min_lat, max_lat, min_lon, max_lon, closed_at, num_changes) " +
      "FROM stdin;\n" );
  }

  shared_ptr<QTextStream> changesetsStream = _outputSections[ApiDb::getChangesetsTableName()].second;
  const QString datestring = QDateTime::currentDateTime().toUTC().toString("yyyy-MM-dd hh:mm:ss.zzz");
  const QString changesetFormat("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\n");

  *changesetsStream <<
    changesetFormat.arg(
      QString::number(_changesetData.currentChangesetId),
      QString::number(_changesetData.changesetUserId),
      datestring,
      QString::number((qlonglong)OsmApiDb::toOsmApiDbCoord(_changesetData.changesetBounds.getMinY())),
      QString::number((qlonglong)OsmApiDb::toOsmApiDbCoord(_changesetData.changesetBounds.getMaxY())),
      QString::number((qlonglong)OsmApiDb::toOsmApiDbCoord(_changesetData.changesetBounds.getMinX())),
      QString::number((qlonglong)OsmApiDb::toOsmApiDbCoord(_changesetData.changesetBounds.getMaxX())),
      datestring,
      QString::number(_changesetData.changesInChangeset));

//  _changesetData.changesetsWritten++;
//  LOG_VART(_changesetData.changesetsWritten);
//  _changesetData.currentChangesetId++;
//  LOG_VART(_changesetData.currentChangesetId);
  _changesetData.changesInChangeset = 0; //??
}

void OsmApiDbBulkWriter::_writeSequenceUpdates(const long changesetId, const long nodeId,
                                               const long wayId, const long relationId)
{
  LOG_TRACE("Writing sequence updates stream...");

  _createTable("sequence_updates", "");

  shared_ptr<QTextStream> sequenceUpdatesStream = _outputSections["sequence_updates"].second;
  const QString sequenceUpdateFormat("SELECT pg_catalog.setval('%1', %2);\n");

  //At least one changeset and some nodes should always be written by a write operation; ways
  //and relations are optional.

  assert(changesetId > 0);
  *sequenceUpdatesStream <<
    sequenceUpdateFormat.arg(ApiDb::getChangesetsSequenceName(), QString::number(changesetId));

  assert(nodeId > 0);
  *sequenceUpdatesStream <<
    sequenceUpdateFormat.arg(ApiDb::getCurrentNodesSequenceName(), QString::number(nodeId));

  if (wayId > 0)
  {
    *sequenceUpdatesStream <<
      sequenceUpdateFormat.arg(ApiDb::getCurrentWaysSequenceName(), QString::number(wayId));
  }

  if (relationId > 0)
  {
    *sequenceUpdatesStream <<
      sequenceUpdateFormat.arg(
        ApiDb::getCurrentRelationsSequenceName(), QString::number(relationId)) << "\n\n";
  }
}

}
