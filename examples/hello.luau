-- Runs in the Nexus standalone runtime: nexus-cli run examples/hello.luau
local function greet(name: string): string
    return `Hello, {name}!`
end

print(greet("Nexus"))

local results = {}
for i = 1, 5 do
    task.spawn(function()
        task.wait(0.01 * i)
        table.insert(results, i * i)
    end)
end
task.wait(0.1)
print("squares:", table.concat(results, ", "))
