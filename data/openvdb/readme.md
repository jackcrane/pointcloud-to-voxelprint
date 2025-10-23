# To process these files

> From the directory data/openvdb (until further notice)

## Install conda

```
brew install --cask miniconda
```

## Create conda environment

```
conda create -n vdb python=3.11 -y
conda init "$(basename $SHELL)"
exec "$SHELL"
conda activate vdb
```

## Install dependencies

```
conda install -c conda-forge pyopenvdb numpy matplotlib tqdm open3d -y

```




## Leave conda environment

```
conda deactivate
```