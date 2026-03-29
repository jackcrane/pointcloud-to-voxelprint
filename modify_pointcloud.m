function slice_pointcloud
clear; clc;

maxPoints = 5e6;

%% ---------- FILE ----------
[fileName, filePath] = uigetfile('*.ply', 'Select a PLY point cloud');
if isequal(fileName,0), error('No file selected.'); end
fullFile = fullfile(filePath, fileName);

%% ---------- LOAD ----------
fid = fopen(fullFile, 'r');
if fid == -1, error('Cannot open file'); end

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

has16 = any(contains(properties,'uint16 red')) || any(contains(properties,'ushort red'));
has8  = any(contains(properties,'uchar red')) || any(contains(properties,'uint8 red'));

raw = textscan(fid, '%f %f %f %f %f %f');
fclose(fid);

X = raw{1}; Y = raw{2}; Z = raw{3};

if has16
    R = raw{4}/65535; G = raw{5}/65535; B = raw{6}/65535;
elseif has8
    R = raw{4}/255; G = raw{5}/255; B = raw{6}/255;
else
    error('Unsupported PLY');
end

%% ---------- DOWNSAMPLE ----------
if numel(X) > maxPoints
    idx = randperm(numel(X), maxPoints);
    X = X(idx); Y = Y(idx); Z = Z(idx);
    R = R(idx); G = G(idx); B = B(idx);
end

%% ---------- LIMITS ----------
xmin = min(X); xmax = max(X);
ymin = min(Y); ymax = max(Y);
zmin = min(Z); zmax = max(Z);

%% ---------- BOUNDARY DETECTION ----------
tol = 1e-6;
isBoundary = ...
    abs(X - xmin) < tol | abs(X - xmax) < tol | ...
    abs(Y - ymin) < tol | abs(Y - ymax) < tol | ...
    abs(Z - zmin) < tol | abs(Z - zmax) < tol;

%% ---------- INITIAL SLICE ----------
bounds = [xmin xmax; ymin ymax; zmin zmax];

% Viewport (what you asked for)
viewLimits = bounds;

%% ---------- FIGURE ----------
fig = figure('Color','w','Units','normalized','Position',[0 0 1 1]);
ax = axes('Parent',fig,'Position',[0.03 0.05 0.65 0.9]);

scatterHandle = scatter3(ax, [], [], [], 1, [], '.');
hold(ax,'on');
boundaryHandle = scatter3(ax, [], [], [], 50, 'r', 'filled');

title(ax,fileName,'Interpreter','none');
xlabel(ax,'X'); ylabel(ax,'Y'); zlabel(ax,'Z');

axis(ax,'equal'); grid(ax,'on'); view(ax,3); rotate3d on;

%% ---------- UI ----------
labels = {'X','Y','Z'};
inputs = cell(3,2);
viewInputs = cell(3,2);

for i = 1:3
    y = 0.75 - (i-1)*0.22;

    % Slice bounds
    inputs{i,1} = uicontrol(fig,'Style','edit','Units','normalized',...
        'Position',[0.72 y 0.07 0.05],'String',num2str(bounds(i,1)),...
        'Callback',@(~,~) update());

    inputs{i,2} = uicontrol(fig,'Style','edit','Units','normalized',...
        'Position',[0.80 y 0.07 0.05],'String',num2str(bounds(i,2)),...
        'Callback',@(~,~) update());

    % Viewport bounds (NEW)
    viewInputs{i,1} = uicontrol(fig,'Style','edit','Units','normalized',...
        'Position',[0.88 y 0.07 0.05],'String',num2str(viewLimits(i,1)),...
        'Callback',@(~,~) updateView());

    viewInputs{i,2} = uicontrol(fig,'Style','edit','Units','normalized',...
        'Position',[0.96 y 0.07 0.05],'String',num2str(viewLimits(i,2)),...
        'Callback',@(~,~) updateView());

    uicontrol(fig,'Style','text','String',[labels{i} ' slice'],...
        'Units','normalized','Position',[0.72 y+0.05 0.15 0.03],'BackgroundColor','w');

    uicontrol(fig,'Style','text','String',[labels{i} ' view'],...
        'Units','normalized','Position',[0.88 y+0.05 0.15 0.03],'BackgroundColor','w');
end

update();
updateView();

%% ---------- FUNCTIONS ----------

function update()
    for k = 1:3
        bounds(k,1) = str2double(get(inputs{k,1},'String'));
        bounds(k,2) = str2double(get(inputs{k,2},'String'));
    end

    mask = X>=bounds(1,1) & X<=bounds(1,2) & ...
           Y>=bounds(2,1) & Y<=bounds(2,2) & ...
           Z>=bounds(3,1) & Z<=bounds(3,2);

    set(scatterHandle,...
        'XData',X(mask),...
        'YData',Y(mask),...
        'ZData',Z(mask),...
        'CData',[R(mask) G(mask) B(mask)]);

    bmask = mask & isBoundary;

    set(boundaryHandle,...
        'XData',X(bmask),...
        'YData',Y(bmask),...
        'ZData',Z(bmask));
end

function updateView()
    for k = 1:3
        viewLimits(k,1) = str2double(get(viewInputs{k,1},'String'));
        viewLimits(k,2) = str2double(get(viewInputs{k,2},'String'));
    end

    xlim(ax, viewLimits(1,:));
    ylim(ax, viewLimits(2,:));
    zlim(ax, viewLimits(3,:));
end

end