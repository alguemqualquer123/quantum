local VirtualJoystick = {}

function VirtualJoystick:init()
    self.active = false
    self.knobEntity = nil
    self.basePos = Vector2(0.0, 0.0)
    self.knobOffset = Vector2(0.0, 0.0)
    self.touchId = nil
    self.joystickDir = Vector2(0.0, 0.0)
end

function VirtualJoystick:update()
    if not self.knobEntity then
        local knob = Engine.scene:findEntity("JoystickKnob")
        if knob then
            self.knobEntity = knob
            local uiComp = UIComponent(self.scene, self.entity)

            uiComp.onDragStart = function(s, x, y)
                self.active = true
                self.basePos = Vector2(x, y)
                local knobObj = Object(self.scene, self.knobEntity)
                knobObj.visible = true
                local baseObj = Object(self.scene, self.entity)
                baseObj.visible = true
            end

            uiComp.onDrag = function(s, x, y)
                if not self.active then return end
                local dx = x - self.basePos.x
                local dy = y - self.basePos.y
                local dist = math.sqrt(dx * dx + dy * dy)
                if dist > self.radius then
                    dx = dx / dist * self.radius
                    dy = dy / dist * self.radius
                    dist = self.radius
                end
                self.knobOffset = Vector2(dx, dy)
                if dist > 0.0 then
                    self.joystickDir = Vector2(dx / self.radius, dy / self.radius)
                else
                    self.joystickDir = Vector2(0.0, 0.0)
                end
            end

            uiComp.onDragEnd = function(s, x, y)
                self.active = false
                self.joystickDir = Vector2(0.0, 0.0)
                local knobObj = Object(self.scene, self.knobEntity)
                knobObj.visible = false
                local baseObj = Object(self.scene, self.entity)
                baseObj.visible = false
            end
        end
    end
end

function VirtualJoystick:getDirection()
    return self.joystickDir
end

return VirtualJoystick
