# Structure library manager

**Purpose:** Make structure definitions easier to organise and discover.

**Description:** The structure view should include a manager showing
which structure definition files are available and which structures are
declared inside each file.

# Configurable structure storage directory

**Purpose:** Let the user control where structure definitions are
stored.

**Description:** breco should allow the user to select the directory
used as the structure storage location.

# Structure-based searching and scanning

**Purpose:** Support specialised binary searches without hardcoding each
format into breco.

**Description:** Conditional structure definitions should be usable as
search or scanning rules. breco should scan data at candidate offsets
and report locations where the structure can be successfully interpreted
and its declared conditions are satisfied.

# Detachable views

**Purpose:** Allow users to arrange the interface across separate
windows or multiple displays.

**Description:** Selected views should be detachable from the main
window and opened as independent windows. When a detached window is
closed, its view should automatically return to its previous position in
the main interface.

# Template-based structure export

**Purpose:** Give users control over the textual representation of
exported structure data.

**Description:** breco should provide a template language for structure
output. When exporting interpreted structures, the user should be able
to select or create a template that defines how the resulting text is
formatted.

# Reusable structure definition includes

**Purpose:** Allow structure declarations to be divided into smaller
reusable components.

**Description:** A structure definition file should be able to include
other structure definition files. Structures and declarations from those
files should be reusable inside larger structure declarations.

# External data-file references

**Purpose:** Support formats whose data is distributed across multiple
related files.

**Description:** A structure definition should be able to declare that a
field, offset, index or other value refers to data in a separate source
file rather than the file currently being examined.

The actual external files should not be hardcoded into the structure
declaration. When the structure is applied or displayed, breco should
prompt the user to select a data file for each declared external-file
role. breco should then use those files when resolving and displaying
the referenced data.
