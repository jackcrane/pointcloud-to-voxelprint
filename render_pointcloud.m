% render_two_ply_equal_scale.m

clear; clc;

files = {
    'ozark_first_1m.ply', 'ozark_first_1m';
    'oz_fm.ply',          'oz_fm';
};

maxPoints = 5e6;
data = cell(2,1);

%% ---------- LOAD ----------
for f = 1:2
    fid = fopen(files{f,1}, 'r');
    if fid == -1
        error('Cannot open file: %s', files{f,1});
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

    has16 = any(contains(properties,'uint16 red'));
    has8  = any(contains(properties,'uchar red'));

    if has16
        raw = textscan(fid, repmat('%f',1,20), numPoints);
        X = raw{1}; Y = raw{2}; Z = raw{3};
        R = raw{18}/65535; G = raw{19}/65535; B = raw{20}/65535;

    elseif has8
        raw = textscan(fid, '%f %f %f %f %f %f %f', numPoints);
        X = raw{1}; Y = raw{2}; Z = raw{3};
        R = raw{4}/255; G = raw{5}/255; B = raw{6}/255;

    else
        error('Unsupported PLY format');
    end

    fclose(fid);

    % Downsample
    if numel(X) > maxPoints
        idx = randperm(numel(X), maxPoints);
        X = X(idx); Y = Y(idx); Z = Z(idx);
        R = R(idx); G = G(idx); B = B(idx);
    end

    data{f} = struct('X',X,'Y',Y,'Z',Z,'C',[R G B]);
end

%% ---------- GLOBAL AXIS (KEY FIX) ----------
allX = [data{1}.X; data{2}.X];
allY = [data{1}.Y; data{2}.Y];
allZ = [data{1}.Z; data{2}.Z];

xmin = min(allX); xmax = max(allX);
ymin = min(allY); ymax = max(allY);
zmin = min(allZ); zmax = max(allZ);

% Prevent flat Z
if zmax == zmin
    zmin = zmin - 1;
    zmax = zmax + 1;
end

% Make cube (critical for equal visual size)
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

tiledlayout(1,2,'TileSpacing','none','Padding','none');

for i = 1:2
    ax = nexttile;

    d = data{i};

    scatter3(ax, d.X, d.Y, d.Z, 1, d.C, '.');
    title(ax, files{i,2});

    % FORCE IDENTICAL SCALE
    xlim(ax, xlim_global);
    ylim(ax, ylim_global);
    zlim(ax, zlim_global);

    axis(ax,'equal');
    axis(ax,'vis3d');
    set(ax,'PlotBoxAspectRatio',[1 1 1]);

    grid(ax,'on');
    view(ax,3);

    xlabel(ax,'X'); ylabel(ax,'Y'); zlabel(ax,'Z');
end

rotate3d on;