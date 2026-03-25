% render_single_ply_with_bounds_ui.m

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

%% ---------- INITIAL BOUNDS ----------
xmin = min(X); xmax = max(X);
ymin = min(Y); ymax = max(Y);
zmin = min(Z); zmax = max(Z);

bounds = [xmin xmax; ymin ymax; zmin zmax];

%% ---------- FIGURE ----------
fig = figure('Color','w','Units','normalized','Position',[0 0 1 1]);

ax = axes('Parent',fig,'Position',[0.05 0.1 0.7 0.85]);

scatterHandle = scatter3(ax, X, Y, Z, 1, [R G B], '.');
title(ax, fileName, 'Interpreter','none');

axis(ax,'equal');
axis(ax,'vis3d');
grid(ax,'on');
view(ax,3);

xlabel(ax,'X'); ylabel(ax,'Y'); zlabel(ax,'Z');

rotate3d on;

%% ---------- UI ----------
labels = {'X','Y','Z'};
inputs = cell(3,2);

for i = 1:3
    y = 0.8 - (i-1)*0.15;

    % min input
    inputs{i,1} = uicontrol(fig,'Style','edit',...
        'Units','normalized','Position',[0.8 y 0.08 0.05],...
        'String',num2str(bounds(i,1)),...
        'Callback',@(src,~) updateBounds());

    % max input
    inputs{i,2} = uicontrol(fig,'Style','edit',...
        'Units','normalized','Position',[0.89 y 0.08 0.05],...
        'String',num2str(bounds(i,2)),...
        'Callback',@(src,~) updateBounds());

    % +
    uicontrol(fig,'Style','pushbutton','String','+',...
        'Units','normalized','Position',[0.8 y-0.06 0.08 0.05],...
        'Callback',@(~,~) shiftBound(i, +1));

    % -
    uicontrol(fig,'Style','pushbutton','String','-',...
        'Units','normalized','Position',[0.89 y-0.06 0.08 0.05],...
        'Callback',@(~,~) shiftBound(i, -1));

    % label
    uicontrol(fig,'Style','text','String',labels{i},...
        'Units','normalized','Position',[0.75 y 0.04 0.05],...
        'BackgroundColor','w','FontWeight','bold');
end

%% ---------- FUNCTIONS ----------

function updateBounds()
    for k = 1:3
        bounds(k,1) = str2double(inputs{k,1}.String);
        bounds(k,2) = str2double(inputs{k,2}.String);
    end

    mask = X >= bounds(1,1) & X <= bounds(1,2) & ...
           Y >= bounds(2,1) & Y <= bounds(2,2) & ...
           Z >= bounds(3,1) & Z <= bounds(3,2);

    scatterHandle.XData = X(mask);
    scatterHandle.YData = Y(mask);
    scatterHandle.ZData = Z(mask);
    scatterHandle.CData = [R(mask) G(mask) B(mask)];

    % keep cube scaling
    dx = bounds(1,2)-bounds(1,1);
    dy = bounds(2,2)-bounds(2,1);
    dz = bounds(3,2)-bounds(3,1);
    d = max([dx dy dz]);

    cx = mean(bounds(1,:));
    cy = mean(bounds(2,:));
    cz = mean(bounds(3,:));

    xlim(ax, cx + [-d/2 d/2]);
    ylim(ax, cy + [-d/2 d/2]);
    zlim(ax, cz + [-d/2 d/2]);
end

function shiftBound(dim, direction)
    range = bounds(dim,2) - bounds(dim,1);
    shift = 0.5 * range * direction;

    bounds(dim,1) = bounds(dim,1) + shift;
    bounds(dim,2) = bounds(dim,2) + shift;

    inputs{dim,1}.String = num2str(bounds(dim,1));
    inputs{dim,2}.String = num2str(bounds(dim,2));

    updateBounds();
end