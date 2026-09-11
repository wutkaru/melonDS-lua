local frame = 0

function _Update()
    frame = frame + 1
    joypad.set({start = frame <= 4})
end
