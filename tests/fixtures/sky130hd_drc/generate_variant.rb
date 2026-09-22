# Copyright 2026 The Design++ Authors

layout = RBA::Layout.new
layout.read($input_gds)
raise 'fixture requires exactly one top cell' unless layout.top_cells.size == 1

if $variant == 'narrow_m1'
  top = layout.top_cell
  center = top.bbox.center
  top.shapes(layout.layer(68, 20)).insert(
    RBA::Box.new(center.x, center.y, center.x + 1, center.y + 1000)
  )
elsif $variant != 'clean'
  raise "unknown fixture variant: #{$variant}"
end

layout.write($output_gds)
