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
 * @copyright Copyright (C) 2016 DigitalGlobe (http://www.digitalglobe.com/)
 */
#ifndef CHANGESETSTATS_H
#define CHANGESETSTATS_H

// hoot
#include <hoot/core/io/ChangesetProvider.h>
#include <hoot/core/elements/ElementType.h>

// Qt
#include <QMap>

namespace hoot
{

/**
 * Simple statistics for an OSM changeset
 */
class ChangesetStats
{

public:

  ChangesetStats();

  QString toString() const;

  long getStat(const QString statName) const { return _stats[statName]; }
  void setStat(const QString statName, const long value) { _stats[statName] = value; }

private:

  QMap<QString, long> _stats;
};

}

#endif // CHANGESETSTATS_H