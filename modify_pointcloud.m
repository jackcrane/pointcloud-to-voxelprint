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

%% ---------- GLOBAL LIMITS ----------
xmin = min(X); xmax = max(X);
ymin = min(Y); ymax = max(Y);
zmin = min(Z); zmax = max(Z);

dx = xmax-xmin; dy = ymax-ymin; dz = zmax-zmin;
d = max([dx dy dz]);

cx = (xmin+xmax)/2;
cy = (ymin+ymax)/2;
cz = (zmin+zmax)/2;

xlim_global = cx + [-d/2 d/2];
ylim_global = cy + [-d/2 d/2];
zlim_global = cz + [-d/2 d/2];

%% ---------- INITIAL SLICE (FULL) ----------
bounds = [xmin xmax; ymin ymax; zmin zmax];

%% ---------- FIGURE ----------
fig = figure('Color','w','Units','normalized','Position',[0 0 1 1]);

ax = axes('Parent',fig,'Position',[0.03 0.05 0.72 0.9]);

scatterHandle = scatter3(ax, [], [], [], 1, [], '.');

title(ax,fileName,'Interpreter','none');
xlabel(ax,'X'); ylabel(ax,'Y'); zlabel(ax,'Z');

axis(ax,'equal');
axis(ax,'vis3d');
grid(ax,'on');
view(ax,3);
rotate3d on;

xlim(ax,xlim_global);
ylim(ax,ylim_global);
zlim(ax,zlim_global);

%% ---------- UI ----------
labels = {'X','Y','Z'};
inputs = cell(3,2);

for i = 1:3
    y = 0.8 - (i-1)*0.18;

    inputs{i,1} = uicontrol(fig,'Style','edit',...
        'Units','normalized','Position',[0.78 y 0.08 0.05],...
        'String',num2str(bounds(i,1)),...
        'Callback',@(~,~) update());

    inputs{i,2} = uicontrol(fig,'Style','edit',...
        'Units','normalized','Position',[0.88 y 0.08 0.05],...
        'String',num2str(bounds(i,2)),...
        'Callback',@(~,~) update());

    uicontrol(fig,'Style','pushbutton','String','+',...
        'Units','normalized','Position',[0.78 y-0.07 0.08 0.05],...
        'Callback',@(~,~) zoomAxis(i,0.5));

    uicontrol(fig,'Style','pushbutton','String','-',...
        'Units','normalized','Position',[0.88 y-0.07 0.08 0.05],...
        'Callback',@(~,~) zoomAxis(i,2.0));

    uicontrol(fig,'Style','text','String',labels{i},...
        'Units','normalized','Position',[0.73 y 0.04 0.05],...
        'BackgroundColor','w','FontWeight','bold');
end

update();

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
    end

    function zoomAxis(dim,factor)
        c = mean(bounds(dim,:));
        half = (bounds(dim,2)-bounds(dim,1))/2;

        newHalf = half * factor;

        bounds(dim,1) = c - newHalf;
        bounds(dim,2) = c + newHalf;

        set(inputs{dim,1},'String',num2str(bounds(dim,1)));
        set(inputs{dim,2},'String',num2str(bounds(dim,2)));

        update();
    end

end