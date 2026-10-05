-- Sculpt Mesh MacroScripts. Copy this file to <3ds Max>\MacroScripts (or your
-- user MacroScripts folder) and assign the actions under
-- Customize > Hotkey Editor / Customize User Interface, category "MLtools".

macroScript Convert_To_Sculpt_Mesh
    category:"MLtools"
    tooltip:"Convert to Sculpt Mesh"
    buttontext:"Convert to Sculpt Mesh"
(
    on isEnabled return selection.count > 0

    on execute do
    (
        undo "Convert to Sculpt Mesh" on
        (
            for o in (selection as array) do convertToSculpt o
        )
        max create mode
        max modify mode
        forceCompleteRedraw()
    )
)

macroScript Toggle_Sculpt_Mode
    category:"MLtools"
    tooltip:"Sculpt Mesh: Toggle Sculpt Mode"
    buttontext:"Sculpt"
(
    on isChecked return (try (SculptMesh.IsSculpting()) catch (false))

    on execute do
    (
        if SculptMesh.IsSculpting() then
            SculptMesh.StopSculpt()
        else if not SculptMesh.StartSculpt() do
            messageBox "Select a single Sculpt Mesh object first." title:"Sculpt Mesh"
    )
)
