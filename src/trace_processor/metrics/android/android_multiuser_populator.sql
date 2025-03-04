--
-- Copyright 2021 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.
--

-- Collect the important timestamps for Multiuser events.
DROP VIEW IF EXISTS multiuser_events;
CREATE VIEW multiuser_events AS
SELECT
      {{start_event}}_time_ns AS event_start_time_ns,
      {{end_event}}_time_ns AS event_end_time_ns
FROM
  (
    SELECT slice.ts AS user_start_time_ns
    FROM slice
    WHERE (slice.name LIKE "onBeforeStartUser%")
    -- TODO: Use a signal based on SysUi so that it covers all switching, not just starting.
  ),
  (
    SELECT slice.ts + slice.dur AS launcher_end_time_ns
    FROM slice
    WHERE (slice.name = "launching: com.google.android.apps.nexuslauncher")
  );

-- Calculation of the duration of the Multiuser event of interest.
DROP VIEW IF EXISTS multiuser_timing;
CREATE VIEW multiuser_timing AS
SELECT
      CAST((event_end_time_ns - event_start_time_ns) / 1e6 + 0.5 AS INT) AS duration_ms
FROM
  multiuser_events;


-- Calculate CPU usage during the Multiuser event of interest.

-- Get all the scheduling slices.
DROP VIEW IF EXISTS sp_sched;
CREATE VIEW sp_sched AS
SELECT ts, dur, cpu, utid
FROM sched;
-- Get all the cpu frequency slices.
DROP VIEW IF EXISTS sp_frequency;
CREATE VIEW sp_frequency AS
SELECT
  ts,
  lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts as dur,
  cpu,
  value as freq_khz
FROM counter
JOIN cpu_counter_track ON counter.track_id = cpu_counter_track.id;
-- Create the span joined table which combines cpu frequency with scheduling slices.
DROP TABLE IF EXISTS sched_with_frequency;
CREATE VIRTUAL TABLE sched_with_frequency
USING SPAN_JOIN(sp_sched PARTITIONED cpu, sp_frequency PARTITIONED cpu);

-- Calculate the CPU cycles spent per process during the duration.
DROP VIEW IF EXISTS cpu_usage_all;
CREATE VIEW cpu_usage_all AS
SELECT
    process.uid / 100000 AS user_id,
    process.name AS process_name,
    SUM(dur * freq_khz) / 1e9 AS cpu_kcycles
FROM
    sched_with_frequency
    JOIN thread USING (utid)
    JOIN process USING (upid)
WHERE
    ts >= (SELECT event_start_time_ns FROM multiuser_events)
    AND
    ts <= (SELECT event_end_time_ns FROM multiuser_events)
GROUP BY upid, process.name
ORDER BY cpu_kcycles DESC;

-- Get the data from cpu_usage_all, but also with the percentage.
DROP VIEW IF EXISTS cpu_usage;
CREATE VIEW cpu_usage AS
SELECT
    user_id,
    process_name,
    CAST(cpu_kcycles / 1e3 AS INT) AS cpu_mcycles,
    cpu_kcycles / (SELECT SUM(cpu_kcycles) FROM cpu_usage_all) * 100 AS cpu_percentage
FROM
    cpu_usage_all
ORDER BY cpu_mcycles DESC LIMIT 6;


-- Record the output for populating the proto.
DROP VIEW IF EXISTS {{output_table_name}};
CREATE VIEW {{output_table_name}} AS
SELECT AndroidMultiuserMetric_EventData(
  'duration_ms', (
    SELECT duration_ms
    FROM multiuser_timing
  ),
  'cpu_usage', (
    SELECT RepeatedField(
      AndroidMultiuserMetric_EventData_CpuUsage(
        'user_id', user_id,
        'process_name', process_name,
        'cpu_mcycles', cpu_mcycles,
        'cpu_percentage', cpu_percentage
      )
    )
    FROM cpu_usage
  )
);
