--[[
Love2D lua script for loading animation from mugen file *.air (animation definition).
It will load atlas image in PNG format with file *.tsv for atlas data.
The image and atlas data is generated using sffcli.exe  (https://github.com/leonkasovan/go-sffcli)
File atlas data has to be edited in last coloumn "Filename_GroupID_ImageNoID" => "GroupID_ImageNoID"

Dhani Novan, 10:21 10 March 2025
Jakarta
]] --
local max_player = 2
local players = {}
local spriteBatch
local atlas_dat = {}
local frame_no = 0
local atlas_img_w, atlas_img_h
local tick = 0
local action_id = 0
local loaded_atlas_img = {}

default_actions = {0,5,6,10,11,12,20,21,40,41,42,43,47,100,105,120,121,122,130,131,132,140,141,142,150,151,152,5000,5001,5002,5005,5006,5007,5010,5011,5012,5015,5016,5017,5020,5021,5022,5025,5026,5027,5030,5040,5050,5070,5080,5090,5100,5160,5170,5110,5120,5200,5210,5300}
-- default_actions = {11, 11, 11, 11, 11, 11, 11}

local gw, gh = love.graphics.getDimensions()
--~ local shader_mask = love.graphics.newCanvas()

--~ love.graphics.setCanvas(shader_mask)
--~ love.graphics.rectangle("fill", 0, 0, gw, gh)
--~ love.graphics.setCanvas()

--~ local shader = love.graphics.newShader("shader.frag")

function trim(s)
	return s:match("^%s*(.-)%s*$")
end

function split(inputstr)
	local t = {}
	for str in string.gmatch(inputstr, "([^,]+)") do
		table.insert(t, trim(str))
	end
	return t
end

-- Check cache for loading atlas image
-- Memory Usage:
-- No cache (always call love.graphics.newImage): 800MB
-- Cache (call loadAtlasImage): 220MB and faster loading time
function loadAtlasImage(filename)
	for k, v in ipairs(loaded_atlas_img) do
		-- if found then use it
		if v.filename == filename then
			return v.image
		end
	end

	-- not found, then load it from disk
	i = {}
	i.filename = filename
	i.image = love.graphics.newImage(filename)
	table.insert(loaded_atlas_img, i)
	return i.image
end

function loadChar(name, x, y)
	player = {}
	player.name = name
	-- player.atlas_img = love.graphics.newImage(player.name .. ".png")
	player.atlas_img = loadAtlasImage(player.name .. ".png")
	player.atlas_dat = {}
	for line in io.lines(player.name .. ".txt") do -- Iterate through each line of player.tsv (tab separated values)
		if #line > 0 then
			src_x, src_y, src_w, src_h, dst_x, dst_y, dst_w, dst_h, spr_off_x, spr_off_y, spr_group_id, spr_img_no = line:match(
				"(%d+)\t(%d+)\t(%d+)\t(%d+)\t(%d+)\t(%d+)\t(%d+)\t(%d+)\t([%-%d]+)\t([%-%d]+)\t(%d+)%D(%d+)")
			src_x = tonumber(src_x)
			src_y = tonumber(src_y)
			src_w = tonumber(src_w)
			src_h = tonumber(src_h)
			dst_x = tonumber(dst_x)
			dst_y = tonumber(dst_y)
			dst_w = tonumber(dst_w)
			dst_h = tonumber(dst_h)
			spr_off_x = tonumber(spr_off_x)
			spr_off_y = tonumber(spr_off_y)
			spr_group_id = tonumber(spr_group_id)
			spr_img_no = tonumber(spr_img_no)

			-- Ensure atlas_dat[group_id] is a table before assigning values
			if player.atlas_dat[spr_group_id] == nil then
				player.atlas_dat[spr_group_id] = {} -- Create a new table for this key
			end
			player.atlas_dat[spr_group_id][spr_img_no] = { src_x, src_y, src_w, src_h, dst_x, dst_y, dst_w, dst_h,spr_off_x, spr_off_y }
		end
	end
	player.atlas_img_w, player.atlas_img_h = player.atlas_img:getDimensions()
	player.sprites = love.graphics.newSpriteBatch(player.atlas_img)
	player.state = 0
	player.x = x
	player.y = y
	player.tick = 0
	player.frame_no = 1

	local action_id = nil
	local new_action_id = nil
	player.anims = {}
	for line in io.lines(player.name .. ".air") do
		line = trim(line)
		if #line == 0 then goto skip end           -- check if empty line
		if string.byte(line, 1) == 59 then goto skip end -- check if commented using ';' = byte 59

		-- parse [Begin Action ID]
		new_action_id = line:match("^%[[Bb][Ee][Gg][Ii][Nn]%s+[Aa][Cc][Tt][Ii][Oo][Nn]%s+(%d+)%]$")

		if new_action_id ~= nil then
			action_id = tonumber(new_action_id)
			goto skip
		end

		-- parse animation element: spr_group_id, spr_img_no, spr_x, spr_y, ticks, flips, color blending
		spr_group_id, spr_img_no, spr_x, spr_y, last_data = line:match(
			"(%d+)%s*,%s*(%d+)%s*,%s*([%d%-]+)%s*,%s*([%d%-]+)%s*,%s*(.-)$")
		if action_id ~= nil and spr_group_id ~= nil and last_data ~= nil then
			spr_group_id = tonumber(spr_group_id)
			spr_img_no = tonumber(spr_img_no)
			spr_x = tonumber(spr_x)
			spr_y = tonumber(spr_y)
			if player.anims[action_id] == nil then
				player.anims[action_id] = {}
			end
			anim = {}
			anim.spr_group_id = spr_group_id
			anim.spr_img_no = spr_img_no
			anim.spr_x = spr_x
			anim.spr_y = spr_y
			anim.ticks = tonumber(last_data)
			if anim.ticks == nil then
				data = split(last_data)
				anim.ticks = tonumber(data[1])
				if anim.ticks == nil then
					print("invalid", line)
				else
					if anim.ticks < 0 then
						anim.ticks = 20
					end
				end
			end
			anim.flip = nil
			anim.blending = nil
			table.insert(player.anims[action_id], anim)
			goto skip
		end

		::skip::
	end

	return player
end

function love.load()
	local windowWidth = 640
	local windowHeight = 480
	love.window.setVSync(1)
	love.window.setTitle("SFF Animations Demo")
	love.window.setMode(windowWidth, windowHeight, {resizable=false, vsync=1})
	table.insert(players, loadChar("sprite_atlas_Super_p13", windowWidth/2, windowHeight/2))
end

function love.update(dt)
	local dt, anim

	for k, player in pairs(players) do
		player.sprites:clear()
		player.tick = player.tick + 1
		if player.anims[player.state] == nil then
			print("error: player.anims[player.state] is nil", player.state)
		else
			if player.anims[player.state][player.frame_no] == nil then
				print("error: layer.anims[player.state][player.frame_no] is nil", player.state, player.frame_no)
			else
				anim = player.anims[player.state][player.frame_no]
				if anim ~= nil then
					if player.tick > anim.ticks then
						player.frame_no = player.frame_no + 1
						if player.anims[player.state] ~= nil and player.frame_no > #player.anims[player.state] then
							player.state = default_actions[math.random(1, #default_actions)]
							player.frame_no = 1
						end
						player.tick = 0
					end
					dt = nil
					if player.state == nil or player.frame_no == nil then
						print("error: player.state or player.frame_no is nil", player.state, player.frame_no)
					else
						dt = player.atlas_dat[anim.spr_group_id][anim.spr_img_no]
					end

					if dt ~= nil then
						player.sprites:add(
							love.graphics.newQuad(dt[1], dt[2], dt[3], dt[4], player.atlas_img_w, player.atlas_img_h),
							player.x + dt[5] + anim.spr_x - dt[9], player.y + dt[6] + anim.spr_y - dt[10])
					else
						print(string.format("%s atlas_dat is nil, state=%d group=%d img_no=%d", player.name, player.state, anim.spr_group_id, anim.spr_img_no))
					end
				else
					print(string.format("anim is nil, state=%d frame=%d", player.state, player.frame_no))
				end
			end
		end
	end
end

function love.draw()
	local windowWidth, windowHeight = love.graphics.getDimensions()
	love.graphics.line(0, windowHeight/2, windowWidth, windowHeight/2)
	love.graphics.line(windowWidth/2, 0, windowWidth/2, windowHeight)

	-- Finally, draw the sprite batch to the screen.
	for k, player in pairs(players) do
		love.graphics.draw(player.sprites)
		love.graphics.print("Action: " .. tostring(player.state), player.x + 5, player.y + 5)
	end
	love.graphics.print("Current FPS: " .. tostring(love.timer.getFPS()), 5, 0)

	local stats = love.graphics.getStats()
	love.graphics.print("Draw Calls: " .. stats.drawcalls, 5, 15)
	love.graphics.print("Texture Memory: " .. tostring(math.floor(stats.texturememory / 1024 / 1024)) .. " MB", 5, 30)
end
