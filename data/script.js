document.addEventListener("DOMContentLoaded", (event) => {
  document.getElementById("currentMode").textContent = data.currentState;
  document.getElementById("digit1").textContent = data.currentDigit1;
  document.getElementById("digit2").textContent = data.currentDigit2;
  document.getElementById("digit3").textContent = data.currentDigit3;
  document.getElementById("digit4").textContent = data.currentDigit4;
  document.getElementById("freeRAM").textContent = data.hFree;
  document.getElementById("contiguousRAM").textContent = data.hMax;
  document.getElementById("fragmentedRAM").textContent = data.hFrag;

  const startTime = document.getElementById("start-time");
  let startDateTime = new Date(0);
  startDateTime.setUTCHours(data.startHour);
  startDateTime.setUTCMinutes(data.startMinute);
  startTime.valueAsDate = startDateTime;
  startTime.addEventListener("input", (event) => {
    data.startHour = event.target.valueAsDate.getUTCHours();
    data.startMinute = event.target.valueAsDate.getUTCMinutes();
    console.log(data.startHour, data.startMinute)
    fetch(`http://${data.hostname}/settings`, 
      {method: "POST", 
      body: new URLSearchParams({
            'startHour': data.startHour,
            'startMinute': data.startMinute
            })
      })
  });

  const endTime = document.getElementById("end-time");
  let endDateTime = new Date(0);
  endDateTime.setUTCHours(data.endHour);
  endDateTime.setUTCMinutes(data.endMinute);
  endTime.valueAsDate = endDateTime;
  endTime.addEventListener("input", (event) => {
    data.endHour = event.target.valueAsDate.getUTCHours();
    data.endMinute = event.target.valueAsDate.getUTCMinutes();
    fetch(`http://${data.hostname}/settings`, 
      {method: "POST", 
      body: new URLSearchParams({
            'endHour': data.endHour,
            'endMinute': data.endMinute
            })
      })
  });

  const brightness = document.getElementById("brightness");
  brightness.value = data.brightness.toString();
  const brightnessValue = document.getElementById("brightness-value");
  brightnessValue.textContent = brightness.value;
  brightness.addEventListener("input", (event) => {
    brightnessValue.textContent = event.target.value;
    data.brightness = event.target.value;
    fetch(`http://${data.hostname}/settings`, 
      {method: "POST", 
      body: new URLSearchParams({
            'brightness': event.target.value
            })
      })
  });

  const hvToggle = document.getElementById("hvBtnToggle");
  hvToggle.checked = data.isHVon? true: false;
  hvToggle.addEventListener("input", (event) => {
    data.isHVOn = event.target.checked;
    fetch(`http://${data.hostname}/settings`, 
      {method: "POST", 
      body: new URLSearchParams({
            'isHVOn': event.target.checked
            })
      })
  });
});
