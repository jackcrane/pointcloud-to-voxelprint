% render_single_ply_equal_scale.m

clear; clc;

maxPoints = 5e6;

%% ---------- FILE PICKER ----------
[fileName, filePath] = uigetfile('*.ply', 'Select a PLY point cloud');

if isequal(fileName,0)
    error('No file selected.');
end

fullFile = fullfile(filePath, fileName);

%% ---------- LOAD ----------
fid = fopen(fullFile, 'r');
if fid == -1
    error('Cannot open file: %s', fullFile);
end

numPoints = 0;
properties = {};

while true
    line = strtrim(fgetl(fid));

    if contains(line,'element vertex')
        numPoints = sscanf(line,'element vertex %d');
    end

    if startsWith(line,'property')
        properties{end+1} = line; %#ok<SAGROW>
    end

    if strcmp(line,'end_header')
        break;
    end
end

has16 = any(contains(properties,'uint16 red')) || ...
        any(contains(properties,'ushort red'));

has8  = any(contains(properties,'uchar red')) || ...
        any(contains(properties,'uint8 red'));

if has16
    raw = textscan(fid, '%f %f %f %f %f %f');
    X = raw{1}; Y = raw{2}; Z = raw{3};
    R = raw{4}/65535; G = raw{5}/65535; B = raw{6}/65535;

elseif has8
    raw = textscan(fid, '%f %f %f %f %f %f');
    X = raw{1}; Y = raw{2}; Z = raw{3};
    R = raw{4}/255; G = raw{5}/255; B = raw{6}/255;

else
    error('Unsupported PLY format');
end

fclose(fid);

%% ---------- DOWNSAMPLE ----------
if numel(X) > maxPoints
    idx = randperm(numel(X), maxPoints);
    X = X(idx); Y = Y(idx); Z = Z(idx);
    R = R(idx); G = G(idx); B = B(idx);
end

%% ---------- GLOBAL AXIS (CUBE SCALING) ----------
xmin = min(X); xmax = max(X);
ymin = min(Y); ymax = max(Y);
zmin = min(Z); zmax = max(Z);

% Prevent flat Z
if zmax == zmin
    zmin = zmin - 1;
    zmax = zmax + 1;
end

dx = xmax - xmin;
dy = ymax - ymin;
dz = zmax - zmin;
d = max([dx dy dz]);

cx = (xmin + xmax)/2;
cy = (ymin + ymax)/2;
cz = (zmin + zmax)/2;

xlim_global = cx + [-d/2 d/2];
ylim_global = cy + [-d/2 d/2];
zlim_global = cz + [-d/2 d/2];

%% ---------- RENDER ----------
figure('Color','w');
ax = axes;

scatter3(ax, X, Y, Z, 1, [R G B], '.');
title(ax, fileName, 'Interpreter', 'none');

xlim(ax, xlim_global);
ylim(ax, ylim_global);
zlim(ax, zlim_global);

axis(ax,'equal');
axis(ax,'vis3d');
set(ax,'PlotBoxAspectRatio',[1 1 1]);

grid(ax,'on');
view(ax,3);

xlabel(ax,'X'); ylabel(ax,'Y'); zlabel(ax,'Z');

rotate3d on;