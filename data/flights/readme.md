# How to process these files

> From the data/flights directory

First, create an opensky network account and request access to their data.

```
trino --user=$USER --password --server=https://trino.opensky-network.org --external-authentication --catalog "minio" --schema "osky"
```

```bash
./download.sh
```

This will create a `flights.csv` file with the flight data. (it will take a few minutes to download)

Then, run the following command to generate the PLY file:

```bash
node flights2csv.js flights.csv out.ply
```

> From the repo root directory

```bash
node index.js data/flights/out.ply air_traffic_out
```

If you want to include the map in the first 100 layers, run:

```bash
node ../pj-voxelprint-colorwheels-v3/4/bump-indexes.js air_traffic_out 100
```

```bash
cp data/flights/stl_area_st.png air_traffic_out/out_1.png
```

```bash
for i in {2..99}; do cp air_traffic_out/out_1.png air_traffic_out/out_${i}.png; done
```

```
https://ows.terrestris.de/osm/service?SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&FORMAT=image/png&TRANSPARENT=FALSE&LAYERS=OSM-WMS&STYLES=&SRS=EPSG:4326&BBOX=-90.797117944,38.415591277,-89.942882056,39.081808723&WIDTH=450&HEIGHT=450
```

```
https://ows.terrestris.de/osm/service?SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&FORMAT=image/png&TRANSPARENT=FALSE&LAYERS=SRTM30-Colored-Hillshade&STYLES=&SRS=EPSG:4326&BBOX=-90.797117944,38.415591277,-89.942882056,39.081808723&WIDTH=450&HEIGHT=450
```