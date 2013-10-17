# Setup MAUI PATH
if ( "$PATH" !~  */maui/sbin* ) then
        setenv PATH "${PATH}:/opt/maui/sbin"
endif
if ( "$PATH" !~  */maui/bin* ) then
        setenv PATH "${PATH}:/opt/maui/bin"
endif

