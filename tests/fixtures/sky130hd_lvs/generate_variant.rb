# Copyright 2026 The Design++ Authors

input = RBA::Layout.new
input.read($input_gds)
raise 'fixture requires exactly one top cell' unless input.top_cells.size == 1

top = input.top_cell
case $variant
when 'clean', 'altered_rule_hash'
  # The clean control is deliberately unchanged.
when 'signal_short'
  top.shapes(input.layer(67, 20)).insert(top.bbox)
when 'signal_open'
  input.each_cell { |cell| cell.shapes(input.layer(66, 44)).clear }
when 'supply_short'
  top.shapes(input.layer(68, 20)).insert(top.bbox)
when 'tap_contact_removed'
  contact = input.layer(66, 44)
  tap = input.layer(65, 44)
  removed = 0
  input.each_cell do |cell|
    contacts = RBA::Region.new(cell.shapes(contact))
    broken = contacts.interacting(RBA::Region.new(cell.shapes(tap)))
    removed += broken.size
    next if broken.is_empty?
    cell.shapes(contact).clear
    cell.shapes(contact).insert(contacts - broken)
  end
  raise 'fixture found no tap contacts to remove' if removed == 0
when 'deep_well'
  top.shapes(input.layer(64, 18)).insert(top.bbox)
when 'missing_boundary'
  input.each_cell { |cell| cell.shapes(input.layer(235, 4)).clear }
else
  raise "unknown fixture variant: #{$variant}"
end

input.write($output_gds)
