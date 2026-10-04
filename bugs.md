# critical inaccuracies
* boats are sometimes appearing on land.  a quick comparison to a comercial AIS system shows our positions are slightly off.  Are we doing lat/lon math correctly?
* the heading line is very nice for boats underway, but the angle of the heading line and boat icon sometimes are off by enough that you can easily see it.

# we have very limited screen real estate, lets optimize
* Can we remove the grey ring around the whole display?  Put the data round the boarder in a shadow box, rounded wtih round ends, but just enough to fit the text.
* Put the zoom controls in the same space around the outside of the display, not inset.

# Make sub-displays, clickable from the main screen
* Tide
  Should open an overlay showing the tide for the last 6 hours and next 24 as a graph
* Vessels
  Should open an overlay with two tabs
    1) vessels
    list of all traffic, nearest to farthest, scrollable
    Columns should be name/id, distance, speed
    2) options
    There should be filter sliders for 'size' and 'speed'
    The size should go from 0ft to 300ft, anything below is filtered
    The speed should go from 0 to 50kts, anything below is filtered
    Add a selector for filter type (label/draw) where label just turns off labels for slow/small vesseles where 'draw' completely removes them from the chat.
    3) A drop down for draw order (speed, size)
       This will make sure the fastest or biggest vessell's label is always visible
    4) add Checkboxes for enabling / disableing the labels on vessels
       one for name
       one for speed

# Add panning map
* Allow the user to drag the map with a single finger drag
* The map maker will need to store bigger maps
* maximum pan area should be the same at all zoom levels (10nm)
* increase the farthest zoomed out to 10nm


---- Set 2 ----
* remove the recenter button, this isn't meant for precise location of the user
* remove the heading lines from boats under 1kt
* add current tide level to the tide bubble (<ft>ft <rise/fall icon> <HH:MM AM/PM> <ft>ft)
* Make everything around the perimeter the same thickness (height of font/bubble) (right now 2nm and the +- around it differ)
* the actual ship icon can still seem to 'stack' in the wrong order (I saw a vessel underway behind a stantionary circle icon)
* the timeout to 'grey out' a vessel seems to short for AIS class B at rest
