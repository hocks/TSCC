# setup maui path
if [ ! `echo $PATH | /bin/grep "maui/sbin"` ]; then
  export PATH=$PATH:/opt/maui/sbin
fi
if [ ! `echo $PATH | /bin/grep "maui/bin"` ]; then
  export PATH=$PATH:/opt/maui/bin
fi

