// Photos (M7): an upload checked by its content, saved in data/photos/,
// and sent back with file.send, which serves only safe types inline.

form.upload = {
    "photo":   {"type": "file", "required": true, "maxKB": 512, "types": ["image/png", "image/jpeg"]},
    "caption": {"type": "text", "max": 80},
}

render = function(req)
    names = []
    if file.exists("photos") then names = file.children("photos")
    shown = []
    for name in names
        shown.push {"name": name}
    end for
    return {"photos": frag("photo", shown), "uploadForm": frag("uploadForm", formState("upload"))}
end function

post.upload = function(req)
    if req.errors then return reshow("uploadForm")
    photo = req.files.photo
    // Our own name, not the visitor's: a count, and the type the content
    // showed.
    ext = ".png"
    if photo.type == "image/jpeg" then ext = ".jpg"
    file.makedir "photos"
    name = str(data.increment("photoCount")) + ext
    file.saveRaw "photos/" + name, photo.data
    session.flash = "Thanks! Saved as " + name + "."
    return redirect("/photos")
end function

// /photos/show?name=1.png. The path can't leave data/ (file checks it).
get.show = function(req)
    if not req.query.hasIndex("name") then return status(404, "No such photo.")
    return file.send("photos/" + req.query.name)
end function
