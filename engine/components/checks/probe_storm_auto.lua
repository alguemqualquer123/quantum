-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Probe: does Lightning's own storm loop fire strikes without anyone calling strike()?
-- The rate is cranked to 600/min so a short headless run has to produce dozens; if this
-- prints 1 or 2, the auto path is dead and the demo only ever looks alive from its driver.
local Kit = require("demos.kit_demo")

Kit.start(function()
    local scene = Kit.createScene()

    local camera = Kit.hold(Camera(scene))
    camera.name = "Camera"
    camera.position = Vector3(0, 10, 30)
    camera.target = Vector3(0, 0, 0)
    scene:setCamera(camera)
    Kit.camera3D(camera, 60, 0.1, 400)

    local weather = Kit.attach(Kit.module("WeatherSystem"), scene, Kit.spawn(scene, "Weather", 0, 0, 0), {
        type = 0, intensity = 1.0, targetIntensity = 1.0, changeSpeed = 10.0,
        toggleKey = 0, cycleKey = 0
    })

    local storm = Kit.attach(Kit.module("Lightning"), scene, Kit.spawn(scene, "Bolt", 0, 0, 0), {
        weather = weather, followWeather = true, stormThreshold = 0.3,
        strikesPerMinute = 600, minDistance = 35, maxDistance = 640, nearDistance = 240,
        thunderPath = "sounds/thunder.ogg", triggerKey = 0
    })

    local driver = { name = "Probe", frame = 0 }

    function driver:onUpdate()
        self.frame = self.frame + 1
        if self.frame % 120 == 0 then
            print(string.format("[probe] frame %d intensity %.2f strikes %d nextAt in %.2f s enabled=%s",
                self.frame, weather:getIntensity(), storm:getStrikeCount(),
                storm.nextAt - Engine.systemTime, tostring(storm.enabled)))
        end
        if self.frame == 600 then
            print(string.format("[probe] done: %d strikes, last %.1f m, %d thunder waits queued",
                storm:getStrikeCount(), storm:getDistance(), #storm.pending))
            System.quit()
        end
    end

    RegisterEngineEvent(driver, "onUpdate")
    Kit.keep[#Kit.keep + 1] = driver
    Kit.report("probe")
end)
