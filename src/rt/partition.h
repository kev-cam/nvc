//
//  Copyright (C) 2026  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#ifndef _RT_PARTITION_H
#define _RT_PARTITION_H

#include "prim.h"

#include <stdbool.h>
#include <stdint.h>

//
// Phase D stage S2a: the PARTITION MAP and BOUNDARY CLASSIFICATION.
//
// This is a pure data model.  Nothing in the scheduler consults it: with
// NVC_PARTITIONS unset (or 0) the map is never built and the only cost is
// one predicted-not-taken load+branch per simulation cycle.  Stage S3
// (schedule-time driver posting, one runner thread per partition) is the
// first consumer.
//
// Terms
// -----
// PARTITION   a set of processes that a single runner thread will own.
//             Partitions are numbered 0 .. part_count()-1.
// GROUP       the indivisible unit of assignment: every process under one
//             rt_scope_t at hierarchy depth NVC_PART_LEVEL (default 5).
//             Module granularity is too coarse -- milestone 0 measured
//             VeeR-EH2 saturating at 2.41x for ANY core count because DEC
//             is one 41.5% group; at level 5 the largest group is 10.7%.
// WEIGHT      the cost estimate a group contributes to its partition.  With
//             no NVC_PART_PROFILE it is the group's process count.  With a
//             profile it is the profile's number, and a group the profile
//             does not name weighs 0 -- a profile is a measurement, and
//             summing a time weight with a count weight in one bin would be
//             meaningless.  profiled_groups in the debug report is the
//             coverage figure to watch.  Groups are packed into partitions
//             by greedy longest-processing-time (LPT), ties broken on group
//             count so an uncovered tail still spreads.
//
// Environment
// -----------
// NVC_PARTITIONS=N        number of partitions; 0/unset = feature off.
// NVC_PART_LEVEL=D        scope depth used as the grouping unit (default 5).
// NVC_PART_PROFILE=path   per-group weights, one per line, any of
//                           "<scope-name> <weight>"
//                           "PD-GRP <i> part=<p> ns=<w> <scope-name>"
//                              (milestone-0 census line)
//                           "PART-GRP <i> part=<p> weight=<w> procs=<n> <name>"
//                              (this report's own per-group line, so a run
//                               feeds the next partitioning unchanged)
//                         '#' comments and blank lines are skipped; repeated
//                         names accumulate.
// NVC_PARTITIONS_DEBUG=1  print the partition table and boundary census;
//                         =2 also lists every group (PART-GRP lines above).
// NVC_PART_HOOK=reset     build at the end of model_reset instead of the end
//                         of the first cycle.  Diagnostic only: it misses the
//                         sensitivity of dynamic-wait processes and the nexus
//                         splits performed by the initial settle.
//

#define PART_MAX_PARTITIONS 64      // masks below are uint64_t
#define PART_NONE           0xffff  // "not assigned" / feature off

// How a nexus relates to the partition boundary.  Determined from the
// partitions of its DRIVERS (sources chain, SOURCE_DRIVER), its READERS
// (pending list, W_PROC only) and its PORT OUTPUTS (outputs chain,
// SOURCE_PORT -> the output nexus and that nexus's readers).
typedef enum {
   // Every driver, reader and port-side reader is in one partition.  Needs
   // no cross-core machinery at all in S3.
   PART_INTERIOR = 0,
   // A process on this nexus's own pending list lives in a partition that
   // none of the drivers is in: the wake itself crosses cores.
   PART_BOUNDARY_DRIVER_READER = 1,
   // The crossing is only through the port/output fan-out: a reader of a
   // port-connected nexus downstream is in another partition.
   PART_BOUNDARY_PORT = 2,
   // No SOURCE_DRIVER contribution at all (port inputs, unconnected
   // signals).  Never a boundary in its own right.
   PART_UNDRIVEN = 3,
} part_class_t;

// Number of partitions requested, or 0 when the feature is off.  Constant
// for the life of the run.
unsigned part_count(void);

// True once the map has been built (end of the first simulation cycle).
// False when the feature is off, and before the build hook fires.
bool part_active(void);

// Partition owning PROC, or PART_NONE when the map is not active.  O(1):
// the id lives in an existing padding hole in rt_proc_t.
unsigned part_of_proc(rt_proc_t *proc);

// Partition that owns N on the DRIVER side -- the partition whose runner
// resolves it -- or PART_NONE if it has no driver in any partition.  For a
// nexus driven from several partitions this is the first driver in the
// sources chain (deterministic; part_report() counts these).
unsigned part_of_nexus(rt_nexus_t *n);

// Boundary classification of N.  Only the boundary nexuses are held in the
// side table (a few thousand out of ~1.3M on VeeR-EH2); INTERIOR and
// UNDRIVEN are recomputed from the short sources chain on a miss.  This is
// a BIND-TIME query, not a hot-path one: S3 discriminates in the hot path
// through the per-instance vtable it installs here, never by looking a
// nexus up.
part_class_t part_class_of_nexus(rt_nexus_t *n);

#endif  // _RT_PARTITION_H
