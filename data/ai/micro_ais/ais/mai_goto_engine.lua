return {
    init = function(ai, existing_engine)

        local engine = existing_engine or {}

        local H = wesnoth.require "lua/helper.lua"
        local AH = wesnoth.require "ai/lua/ai_helper.lua"
        local LS = wesnoth.require "lua/location_set.lua"

        function engine:mai_goto_eval(cfg)

            -- If cfg.release_all_units_at_goal is set, check
            -- whether the goal has already been reached, in
            -- which case we do not do anything
            if cfg.release_all_units_at_goal then
                for rel in H.child_range(self.data, "goto_release_all") do
                    if (rel.id == cfg.ca_id) then
                        return 0
                    end
                end
            end

            -- For convenience, we check for locations here, and just pass that to the exec function
            -- This is mostly to make the unique_goals option easier
            local width, height = wesnoth.get_map_size()
            local locs = wesnoth.get_locations {
                x = '1-' .. width,
                y = '1-' .. height,
                { "and", cfg.filter_location }
            }
            --print('#locs org', #locs)
            if (#locs == 0) then return 0 end

            -- If 'unique_goals' is set, check whether there are locations left to go to
            if cfg.unique_goals then
                -- First, some cleanup of previous turn data
                local str = 'goals_taken_' .. (wesnoth.current.turn - 1)
                self.data[str] = nil

                -- Now on to the current turn
                local str = 'goals_taken_' .. wesnoth.current.turn
                for i = #locs,1,-1 do
                    if self.data[str] and self.data[str]:get(locs[i][1], locs[i][2]) then
                        table.remove(locs, i)
                    end
                end
            end
            --print('#locs mod', #locs)
            if (not locs[1]) then return 0 end

            -- Find the goto units
            local units = wesnoth.get_units { side = wesnoth.current.side,
                { "and", cfg.filter }, formula = '$this_unit.moves > 0'
            }

            -- Exclude released units
            if cfg.release_unit_at_goal then
                for i_unit=#units,1,-1 do
                    for rel in H.child_range(self.data, "goto_release_unit") do
                        if (rel.id == cfg.ca_id .. '_' .. units[i_unit].id) then
                           table.remove(units, i_unit)
                           break
                        end
                    end
                end
            end
            if (not units[1]) then return 0 end

            -- Now store units and locs in self.data, so that we don't need to duplicate this in the exec function
            self.data.units, self.data.locs = units, locs

            return cfg.ca_score or 210000
        end

        function engine:mai_goto_exec(cfg)
            local units, locs = self.data.units, self.data.locs  -- simply for convenience

            local closest_hex, best_unit, max_rating = {}, {}, -9e99
            for i,u in ipairs(units) do
                for i,l in ipairs(locs) do

                    -- If use_straight_line is set, we simply find the closest
                    -- hex to the goal that the unit can get to
                    if cfg.use_straight_line then
                        local hex, unit, rating = AH.find_best_move(u, function(x, y)
                            local r = - H.distance_between(x, y, l[1], l[2])
                            -- Also add distance from unit as very small rating component
                            -- This is mostly here to keep unit in place when no better hexes are available
                            r = r - H.distance_between(x, y, u.x, u.y) / 1000.
                            return r
                        end, { no_random = true })

                        if (rating > max_rating) then
                            max_rating = rating
                            closest_hex, best_unit = hex, u
                        end
                    else  -- Otherwise find the best path to take
                        local path, cost = wesnoth.find_path(u, l[1], l[2])

                        -- Make all hexes within the unit's current MP equaivalent
                        if (cost <= u.moves) then cost = 0 end

                        rating = - cost

                        -- Add a small penalty for occupied hexes
                        -- (this mean occupied by an allied unit, as enemies make the hex unreachable)
                        local unit_in_way = wesnoth.get_unit(l[1], l[2])
                        if unit_in_way and ((unit_in_way.x ~= u.x) or (unit_in_way.y ~= u.y)) then
                            rating = rating - 0.01
                        end

                        if (rating > max_rating) then
                            max_rating = rating
                            closest_hex, best_unit = l, u
                        end
                    end
                end
            end
            --print(best_unit.id, best_unit.x, best_unit.y, closest_hex[1], closest_hex[2], max_rating)

            AH.movefull_outofway_stopunit(ai, best_unit, closest_hex[1], closest_hex[2])

            -- If 'unique_goals' is set, mark this location as being taken
            if cfg.unique_goals then
                local str = 'goals_taken_' .. wesnoth.current.turn
                if (not self.data[str]) then self.data[str] = LS.create() end
                self.data[str]:insert(closest_hex[1], closest_hex[2])
            end

            -- If release_unit_at_goal= or release_all_units_at_goal= key is set:
            -- Check if the unit made it to one of the goal hexes
            -- This needs to be done for the original goal hexes, not checking the SLF again,
            -- as that might have changed based on the new situation on the map
            if cfg.release_unit_at_goal or cfg.release_all_units_at_goal then
                local unit_at_goal = false
                for i,l in ipairs(locs) do
                    if (best_unit.x == l[1]) and (best_unit.y == l[2]) then
                        unit_at_goal = true
                        break
                    end
                end

                -- If a unit was found, mark either it or all units as released
                -- Needs to be stored persistently in self.data meaning:
                -- 1. Needs to be in WML table format
                -- 2. Keys cannot contain certain characters -> everything potentially user-defined needs to be in values
                if unit_at_goal then
                    if cfg.release_unit_at_goal then
                        table.insert(self.data, { "goto_release_unit" , { id = cfg.ca_id .. '_' .. best_unit.id } } )
                    end

                    if cfg.release_all_units_at_goal then
                        --print("Releasing all units")
                        table.insert(self.data, { "goto_release_all", { id = cfg.ca_id } } )
                    end
                end
            end

            -- And some cleanup
            self.data.units, self.data.locs = nil, nil
        end

        return engine
    end
}
