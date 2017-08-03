-- Car profile

api_version = 1

local find_access_tag = require("lib/access").find_access_tag
local Set = require('lib/set')
local Sequence = require('lib/sequence')
local Handlers = require("lib/handlers")
local Measure = require("lib/measure")
local next = next       -- bind to local for speed

-- set profile properties
properties.max_speed_for_map_matching           = 180/3.6 -- 180kmph -> m/s
properties.use_turn_restrictions                = true
properties.continue_straight_at_waypoint        = true
properties.left_hand_driving                    = false
-- For routing based on duration, but weighted for preferring certain roads
properties.weight_name                          = 'routability'
-- For shortest duration without penalties for accessibility
--properties.weight_name                        = 'duration'
-- For shortest distance without penalties for accessibility
--properties.weight_name                        = 'distance'

-- Set to true if you need to call the node_function for every node.
-- Generally can be left as false to avoid unnecessary Lua calls
-- (which slow down pre-processing).
properties.call_tagless_node_function      = false


local profile = {
  default_mode      = mode.driving,
  default_speed     = 10,
  oneway_handling   = true,

  side_road_multiplier       = 0.8,
  turn_penalty               = 15,
  speed_reduction            = 0.8,
  traffic_light_penalty      = 2,
  u_turn_penalty             = 30,

  -- bounds of truck in meters
  truck_length = 18.75,
  truck_height = 5,
  truck_width = 2.6,
  truck_weight = 44000,

  -- Note: this biases right-side driving.
  -- Should be inverted for left-driving countries.
  turn_bias   = properties.left_hand_driving and 1/1.075 or 1.075,

  -- a list of suffixes to suppress in name change instructions
  suffix_list = {
    'N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW', 'North', 'South', 'West', 'East'
  },

  barrier_whitelist = Set {
    'cattle_grid',
    'border_control',
    'checkpoint',
    'toll_booth',
    'sally_port',
    'gate',
    'lift_gate',
    'no',
    'entrance'
  },

  access_tag_whitelist = Set {
    'yes',
    'motorcar',
    'motor_vehicle',
    'vehicle',
    'permissive',
    'designated',
    'hov'
  },

  access_tag_blacklist = Set {
    'no',
    'agricultural',
    'forestry',
    'emergency',
    'psv',
    'customers',
    'private',
    'delivery',
    'destination'
  },

  restricted_access_tag_list = Set {
    'private',
    'delivery',
    'destination',
    'customers',
  },

  access_tags_hierarchy = Sequence {
    'motorcar',
    'motor_vehicle',
    'vehicle',
    'access',
  },

  hgv_tags_whitelist = Set {
    'yes',
    'motorcar',
    'motor_vehicle',
    'vehicle',
    'permissive',
    'designated',
    'hov',
    'agricultural',
    'forestry',
  },

  service_tag_forbidden = Set {
    'emergency_access'
  },

  restrictions = Sequence {
    'motorcar',
    'motor_vehicle',
    'vehicle'
  },

  avoid = Set {
    'area',
    -- 'toll',    -- uncomment this to avoid tolls
    'reversible',
    'impassable',
    'hov_lanes',
    'steps',
    'construction',
    'proposed'
  },

  speeds = Sequence {
    highway = {
      motorway        = 90,
      motorway_link   = 45,
      trunk           = 85,
      trunk_link      = 40,
      primary         = 65,
      primary_link    = 30,
      secondary       = 55,
      secondary_link  = 25,
      tertiary        = 40,
      tertiary_link   = 20,
      unclassified    = 25,
      residential     = 25,
      living_street   = 10,
      service         = 15
    }
  },

  service_penalties = {
    alley             = 0.5,
    parking           = 0.5,
    parking_aisle     = 0.5,
    driveway          = 0.5,
    ["drive-through"] = 0.5,
    ["drive-thru"] = 0.5
  },

 restricted_highway_whitelist = Set {
      'motorway',
      'motorway_link',
      'trunk',
      'trunk_link',
      'primary',
      'primary_link',
      'secondary',
      'secondary_link',
      'tertiary',
      'tertiary_link',
      'residential',
      'living_street',
  },

  route_speeds = {
    ferry = 5,
    shuttle_train = 10
  },

  bridge_speeds = {
    movable = 5
  },

  -- surface/trackype/smoothness
  -- values were estimated from looking at the photos at the relevant wiki pages

  -- max speed for surfaces
  surface_speeds = {
    asphalt = nil,    -- nil mean no limit. removing the line has the same effect
    concrete = nil,
    ["concrete:plates"] = nil,
    ["concrete:lanes"] = nil,
    paved = nil,

    cement = 80,
    compacted = 80,
    fine_gravel = 80,

    paving_stones = 60,
    metal = 60,
    bricks = 60,

    grass = 40,
    wood = 40,
    sett = 40,
    grass_paver = 40,
    gravel = 40,
    unpaved = 40,
    ground = 40,
    dirt = 40,
    pebblestone = 40,
    tartan = 40,

    cobblestone = 30,
    clay = 30,

    earth = 20,
    stone = 20,
    rocky = 20,
    sand = 20,

    mud = 10
  },

  -- max speed for tracktypes
  tracktype_speeds = {
    grade1 =  60,
    grade2 =  40,
    grade3 =  30,
    grade4 =  25,
    grade5 =  20
  },

  -- max speed for smoothnesses
  smoothness_speeds = {
    intermediate    =  80,
    bad             =  40,
    very_bad        =  20,
    horrible        =  10,
    very_horrible   =  5,
    impassable      =  0
  },

  -- http://wiki.openstreetmap.org/wiki/Speed_limits
  maxspeed_table_default = {
    urban = 50,
    rural = 90,
    trunk = 110,
    motorway = 130
  },

  -- List only exceptions
  maxspeed_table = {
    ["ch:rural"] = 80,
    ["ch:trunk"] = 100,
    ["ch:motorway"] = 120,
    ["de:living_street"] = 7,
    ["ru:living_street"] = 20,
    ["ru:urban"] = 60,
    ["ua:urban"] = 60,
    ["at:rural"] = 100,
    ["de:rural"] = 100,
    ["at:trunk"] = 100,
    ["cz:trunk"] = 0,
    ["ro:trunk"] = 100,
    ["cz:motorway"] = 0,
    ["de:motorway"] = 0,
    ["ru:motorway"] = 110,
    ["gb:nsl_single"] = (60*1609)/1000,
    ["gb:nsl_dual"] = (70*1609)/1000,
    ["gb:motorway"] = (70*1609)/1000,
    ["uk:nsl_single"] = (60*1609)/1000,
    ["uk:nsl_dual"] = (70*1609)/1000,
    ["uk:motorway"] = (70*1609)/1000,
    ["nl:rural"] = 80,
    ["nl:trunk"] = 100,
    ["none"] = 140
  },

  truck_penalties = {
    living_street   = 0.1,
    residential     = 0.1,
    service         = 0.1,
    street          = 0.1,
    unclassified    = 0.1,

    secondary       = 0.5
  }
}

function truck_penalties(profile,way,result,data)
  local truck_penalty = 1.0
  local hw = way:get_value_by_key("highway")
  if hw and profile.truck_penalties[hw] then
    truck_penalty = profile.truck_penalties[hw]

--print(truck_penalty)

    if result.forward_speed > 0 then
      if not result.forward_rate then
        result.forward_rate = (result.forward_speed * truck_penalty) / 3.6
      else
        result.forward_rate = math.min(result.forward_rate, (result.forward_speed * truck_penalty) / 3.6)
      end
    end
    if result.backward_speed > 0 then
      if not result.backward_rate then
        result.backward_rate = (result.backward_speed * truck_penalty) / 3.6
      else
        result.backward_rate = math.min(result.backward_rate, (result.backward_speed * truck_penalty) / 3.6)
      end
    end
    if result.duration > 0 then
      if not result.weight then
        result.weight = result.duration / truck_penalty
      else
        result.weight = math.max(result.weight, result.duration / truck_penalty)
      end
    end
  end
end

function get_name_suffix_list(vector)
  for index,suffix in ipairs(profile.suffix_list) do
      vector:Add(suffix)
  end
end

function get_restrictions(vector)
  for i,v in ipairs(profile.restrictions) do
    vector:Add(v)
  end
end

function node_function (node, result)
  -- parse access and barrier tags
  local access = find_access_tag(node, profile.access_tags_hierarchy)
  if access then
    if profile.access_tag_blacklist[access] and not profile.restricted_access_tag_list[access] then
      result.barrier = true
    end
  else
    local barrier = node:get_value_by_key("barrier")
    if barrier then
      --  make an exception for rising bollard barriers
      local bollard = node:get_value_by_key("bollard")
      local rising_bollard = bollard and "rising" == bollard

      if not profile.barrier_whitelist[barrier] and not rising_bollard then
        result.barrier = true
      end
    end
  end

  -- check if node is a traffic light
  local tag = node:get_value_by_key("highway")
  if "traffic_signals" == tag then
    result.traffic_lights = true
  end
end

function way_function(way, result)
  -- the intial filtering of ways based on presence of tags
  -- affects processing times significantly, because all ways
  -- have to be checked.
  -- to increase performance, prefetching and intial tag check
  -- is done in directly instead of via a handler.

  -- in general we should  try to abort as soon as
  -- possible if the way is not routable, to avoid doing
  -- unnecessary work. this implies we should check things that
  -- commonly forbids access early, and handle edge cases later.

  -- data table for storing intermediate values during processing
  local data = {
    -- prefetch tags
    highway = way:get_value_by_key('highway'),
    bridge = way:get_value_by_key('bridge'),
    route = way:get_value_by_key('route'),
    hgv = way:get_value_by_key('hgv'),
  }

  -- perform an quick initial check and abort if the way is
  -- obviously not routable.
  -- highway or route tags must be in data table, bridge is optional
  if (not data.highway or data.highway == '') and
  (not data.route or data.route == '')
  then
    return
  end

  --- check trucks
  if data.hgv and not profile.hgv_tags_whitelist[data.hgv] then
    return
  end

  -- check height
  way_height = Measure.get_max_height(way)
  if way_height then
    if way_height <= profile.truck_height then
      return
    end
  end

  -- check width
  way_width = Measure.get_max_width(way)
  if way_width then
    if way_width <= profile.truck_width then
      return
    end
  end

  -- check maxweight
  way_weight = Measure.get_max_weight(way)
  if way_weight then
    if way_weight <= profile.truck_weight then
      return
    end
  end
  

  handlers = Sequence {
    -- set the default mode for this profile. if can be changed later
    -- in case it turns we're e.g. on a ferry
    'handle_default_mode',

    -- check various tags that could indicate that the way is not
    -- routable. this includes things like status=impassable,
    -- toll=yes and oneway=reversible
    'handle_blocked_ways',

    -- determine access status by checking our hierarchy of
    -- access tags, e.g: motorcar, motor_vehicle, vehicle
    'handle_access',

    -- check whether forward/backward directions are routable
    'handle_oneway',

    -- check a road's destination
    'handle_destinations',

    -- check whether we're using a special transport mode
    'handle_ferries',
    'handle_movables',

    -- handle service road restrictions
    'handle_service',

    -- handle hov
    'handle_hov',

    -- compute speed taking into account way type, maxspeed tags, etc.
    'handle_speed',
    'handle_surface',
    'handle_maxspeed',
    'handle_penalties',

    -- compute class labels
    'handle_classes',

    -- handle turn lanes and road classification, used for guidance
    'handle_turn_lanes',
    'handle_classification',

    -- handle various other flags
    'handle_roundabouts',
    'handle_startpoint',

    -- set name, ref and pronunciation
    'handle_names',

    -- set weight properties of the way
    'handle_weights'
  }

  Handlers.run(handlers,way,result,data,profile)

  truck_penalties(profile,way,result,data)
end

function turn_function (turn)
  -- Use a sigmoid function to return a penalty that maxes out at turn_penalty
  -- over the space of 0-180 degrees.  Values here were chosen by fitting
  -- the function to some turn penalty samples from real driving.
  local turn_penalty = profile.turn_penalty
  local turn_bias = profile.turn_bias

  if turn.has_traffic_light then
      turn.duration = profile.traffic_light_penalty
  end

  if turn.turn_type ~= turn_type.no_turn then
    if turn.angle >= 0 then
      turn.duration = turn.duration + turn_penalty / (1 + math.exp( -((13 / turn_bias) *  turn.angle/180 - 6.5*turn_bias)))
    else
      turn.duration = turn.duration + turn_penalty / (1 + math.exp( -((13 * turn_bias) * -turn.angle/180 - 6.5/turn_bias)))
    end

    if turn.direction_modifier == direction_modifier.u_turn then
      turn.duration = turn.duration + profile.u_turn_penalty
    end
  end

  -- for distance based routing we don't want to have penalties based on turn angle
  if properties.weight_name == 'distance' then
     turn.weight = 0
  else
     turn.weight = turn.duration
  end

  if properties.weight_name == 'routability' then
      -- penalize turns from non-local access only segments onto local access only tags
      if not turn.source_restricted and turn.target_restricted then
          turn.weight = properties.max_turn_weight;
      end
  end
end
