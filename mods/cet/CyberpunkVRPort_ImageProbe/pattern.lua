-- Pure geometry; points are in metres relative to a fixed world-space anchor.
-- This is an unlit geometry probe, NOT a replacement for lit-material tests.
local M = {}

function M.build(distance, phase)
    assert(type(distance) == 'number' and distance >= 1 and distance <= 12)
    phase = phase or 0
    local lines = {}
    local function line(a, b, color)
        lines[#lines + 1] = { a = a, b = b, color = color or 'white' }
    end
    -- Fixed-size planar grid: changing distance changes its angular frequency.
    for i = -6, 6 do
        local x = i * 0.25
        line({x, distance, -1}, {x, distance, 1}, i == 0 and 'red' or 'gray')
    end
    for i = -4, 4 do
        local z = i * 0.25
        line({-1.5, distance, z}, {1.5, distance, z}, i == 0 and 'green' or 'gray')
    end
    -- Dense vertical and diagonal edges for temporal shimmer/eye disparity.
    for i = 0, 12 do
        local x = -1.3 + i * 0.035
        line({x, distance - 0.2, -0.65}, {x, distance - 0.2, 0.65})
    end
    line({0.45, distance - 0.2, -0.7}, {1.25, distance - 0.2, 0.7})
    line({0.45, distance - 0.2, 0.7}, {1.25, distance - 0.2, -0.7})
    -- A foreground wire cube makes incorrect mono output recognizable.
    local cx = 0.55 * math.sin(phase)
    local cy = distance * 0.55
    local half = 0.18
    for axis = 1, 3 do
        local b = axis % 3 + 1
        local c = b % 3 + 1
        for sb = -1, 1, 2 do
            for sc = -1, 1, 2 do
                local a = {cx, cy, 0}
                local z = {cx, cy, 0}
                a[axis] = a[axis] - half
                z[axis] = z[axis] + half
                a[b], z[b] = a[b] + sb * half, z[b] + sb * half
                a[c], z[c] = a[c] + sc * half, z[c] + sc * half
                line(a, z, 'cyan')
            end
        end
    end
    return lines
end

function M.world(anchor, point)
    local p, r, f, u = anchor.position, anchor.right, anchor.forward, anchor.up
    return {
        x = p.x + r.x * point[1] + f.x * point[2] + u.x * point[3],
        y = p.y + r.y * point[1] + f.y * point[2] + u.y * point[3],
        z = p.z + r.z * point[1] + f.z * point[2] + u.z * point[3]
    }
end

return M
