trino --user=jackcrane --password --server=https://trino.opensky-network.org \
  --external-authentication --catalog "minio" --schema "osky" \
  --execute "
WITH arrivals AS (
  SELECT icao24, callsign, firstseen, lastseen
  FROM flights_data4
  WHERE estarrivalairport = 'KSTL'
    AND firstseen >= to_unixtime(TIMESTAMP '2025-10-01 00:00:00 UTC')
    AND firstseen <  to_unixtime(TIMESTAMP '2025-10-21 00:00:00 UTC')
  ORDER BY firstseen
  LIMIT 1000
),
tracks AS (
  SELECT
    s.icao24,
    a.callsign,
    s.time,
    s.lat,
    s.lon,
    s.baroaltitude AS altitude
  FROM state_vectors_data4 s
  JOIN arrivals a
    ON s.icao24 = a.icao24
    AND s.time BETWEEN a.firstseen AND a.lastseen
  WHERE
    s.onground = false
    AND s.lat IS NOT NULL
    AND s.lon IS NOT NULL
    AND s.baroaltitude IS NOT NULL
    AND s.baroaltitude < 3048  -- below 10,000 ft
)
SELECT *
FROM tracks
WHERE
  lat BETWEEN 38.7487 - 0.333108723 AND 38.7487 + 0.333108723
  AND lon BETWEEN -90.3700 - 0.427117944 AND -90.3700 + 0.427117944
ORDER BY icao24, time;
" > flights.csv